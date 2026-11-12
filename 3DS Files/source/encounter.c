#include "encounter.h"
#include "aidriver.h"
#include "difficulty.h"
#include "gps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// RandomEncounterState (SpeedPS.elf 0x801277C0..0x80127B90).
#define ENC_INIT_SEC       30.0f   // Init's timer, DAT_80329428
#define ENC_RESET_SEC      0.0f    // Reset's, DAT_8032942C
#define ENC_RACE_OUTRUN    7       // the race type Tick sets

// RandomEncounterManager (0x803189B0 on).
#define ENC_PLACE_GAP_SEC  5.0f    // DAT_803189BC: since the last car was placed
#define ENC_PLACE_MIN_M    250.0f  // DAT_803189C4: the first guess, and the player's distance
#define ENC_PLACE_MAX_M    500.0f  // DAT_803189C8: how far the search walks
#define ENC_CLEAR_CAR_M    30.0f   // DAT_803189CC: TooCloseToAnotherCar for any car
#define ENC_CLEAR_RACER_M  300.0f  // DAT_803189D0: and for an AI racer (driver type 2)
#define ENC_PLACE_STEP_M   50.0f   // DAT_803189E8
#define ENC_PURSUE_AHEAD_M 10.0f   // DAT_803189E0: SetOffsetPursuit's longitudinal offset
#define ENC_OLD_SEC        30.0f   // DAT_803189FC
#define ENC_OLD_M          250.0f  // DAT_80318A00
// MakeOpponentsDriveNearPlayer: within this of the player the car takes a
// side, 30 m while it is behind or ahead (mode 1) and 20 m once beside, and
// holds it unless it is 2 m over on the other.
#define ENC_NEAR_AHEAD_M   30.0f
#define ENC_NEAR_BESIDE_M  20.0f
#define ENC_SIDE_M         5.0f
#define ENC_SIDE_SWAP_M    2.0f

// AIGoalOffsetPursuit::GetSpeedLimit in free roam (0x803F4494 clear): the
// graph at 0x80318BE4 of the 2D distance to the target, times its speed, and
// an AI racer's floor -- 60 mph within 100 m (DAT_803161B8), 40 beyond.
#define ENC_FLOOR_NEAR     26.8216f
#define ENC_FLOOR_FAR      17.8811f
#define ENC_FLOOR_NEAR_M   100.0f
static const float kSpeedGraph[5][2] = {
	{ 0.0f, 1.1f }, { 20.0f, 1.0f }, { 50.0f, 0.8f }, { 100.0f, 0.5f }, { 150.0f, 0.1f }
};

// AIActionDriveAtOffset (0x80316D30 on): alongside once the error is under
// 30 m, back to the best line over 50, and the target's speed 2 m/s either
// way while the error is 2 m or more.
#define ENC_ALONG_IN_M     30.0f
#define ENC_ALONG_OUT_M    50.0f
#define ENC_ALONG_ERR_M    2.0f
#define ENC_ALONG_DV       2.0f
// PORT: the action drops when the target is not in the car's
// NearbyAvoidableList, whose radius is not decoded.
#define ENC_AVOIDABLE_M    60.0f

// Think's mover timer (opponent.c PHYS_HOLD_SEC / PHYS_REARM_SEC).
#define ENC_PHYS_HOLD_SEC  5.0f
#define ENC_PHYS_REARM_SEC 1.0f

// PORT: the line walked ahead, the span its curvature is read over (aidriver.c
// AISPEED_CURV_SPAN), the off-page mover's pedals and the physics range's
// hysteresis.
#define ENC_AHEAD_M        320.0f
#define ENC_CURV_SPAN      15.0f
// AIActionDriveAtOffset's SetChoice (0x8001573C): the drive target moves
// across at this many metres a metre (0x80316D38). The other choices' rates
// are AvoidAvoidable's (opponent.h OPPONENT_AVOID_*).
#define ENC_OFFSET_RATE    0.15f
#define ENC_FAR_ACCEL      4.0f
#define ENC_PHYS_OUT_M     (ENCOUNTER_PHYS_M + 20.0f)
// The free-roam racers are RaceCarControllers (DecideBoostFactor below), so
// AIGoalOffsetPursuit's ChooseNewAction asks AIActionGetUnstuck first, as a
// race's racers do (SpeedPS 0x80316xxx; opponent.c's helpers, TASKS.md #517),
// and RaceCarController::Think's very-stuck test resets a car that averaged
// under 0.2 m/s along its line over ten 1.5 s samples with no player within
// 50 m (OPPONENT_STUCK_*). No race runs in free roam, so GracePeriodIsActive
// never holds GetUnstuck back here.
#define ENC_FALL_M         15.0f
// AIGoalOffsetPursuit's ChooseNewAction (0x8000D320) then asks for
// AIActionIJustGotHit and AIActionAvoidAvoidable, as ChooseNewActionAsRacer
// (the getaway's goals) does: the avoid is opponent.c's core (encAvoid).
// PORT: IJustGotHit and AccidentRecovery are not ported here.

// The AIGoal types the racers take (AICarController +0x704, the goal's +0x14).
#define ENC_GOAL_PURSUIT   9    // AIGoalOffsetPursuit
#define ENC_GOAL_GETAWAY   6    // AIGoalGetAwayFromCar
#define ENC_GOAL_CHASE     5    // AIGoalChaseCar

// GetCarThatCanBeEngaged (0x8003DAB0), its constants at 0x80318A04 on.
#define ENC_ENGAGE_M       30.0f    // DAT_80318A18: within this of the player
#define ENC_ENGAGE_SLACK   10.0f    // DAT_80318A1C: the looser limits' distance, and the angle's
#define ENC_ENGAGE_DZ_M    5.0f     // DAT_80318A20
#define ENC_MPH            2.237f   // DAT_80318A24
#define ENC_ENGAGE_MPH     30.0f    // DAT_80318A18 again: the car's speed at least
#define ENC_ENGAGE_DV_LO   (-10.0f) // DAT_80318A28: the car's mph less the player's
#define ENC_ENGAGE_DV_HI   10.0f    // DAT_80318A1C
#define ENC_ENGAGE_DV_SLACK 5.0f    // DAT_80318A20
#define ENC_ENGAGE_DEG     10.0f    // DAT_80318A1C: the headings' angle under this
#define ENC_DIR_MIN_SPEED  1.0f     // DAT_80318A10: slower, a car's forward axis is its direction

// The getaway.
#define ENC_GETAWAY_M      300.0f   // GetGetAwayDistance, DAT_8033B174 (metric)
#define ENC_GETAWAY_FT_M   304.9f   // DAT_8033B178: 1000 ft
#define ENC_GAP_SLOW       10.0f    // UpdateGetAway, DAT_8033AEDC: m/s
#define ENC_GAP_POINT_M    30.0f    // DAT_8033AEEC: the follower within this of the leader's route point
#define ENC_BLINK_SEC      2.0f     // BlinkBlinkPoof's StartBlinking, DAT_8033A55C
#define ENC_CLEAR_M        250.0f   // TryAndDeactivateAllActiveOpponents, DAT_80318A04
// DecideSignpostAsGetAwayCar (0x8000ACFC), constants at 0x80316024 on.
#define ENC_TAKE_NEAR_M    150.0f   // DAT_80316038: the leader within this of racer 1's car
#define ENC_TAKE_NEAR      0.75f    // DAT_8031603C
#define ENC_TAKE_FAR       0.1f     // DAT_80316040
#define ENC_TAKE_4_SET     0.333f   // DAT_80316044: a type 4 sign with bits 26 and 27 set
#define ENC_TAKE_4_CLEAR   0.5f     // DAT_8031602C: bit 26 set, 27 clear
// DoPathfinding (0x80029D2C): FindPathToTarget's reach, DAT_80317CA4. PORT:
// AIGoalChaseCar thinks with the AI's think rate, which is not decoded; the
// port looks for the path again every ENC_CHASE_THINK_SEC, from the car's
// next route point, over at most ENC_SEARCH_MAX route points.
#define ENC_CHASE_SEARCH_M 400.0f
#define ENC_CHASE_THINK_SEC 1.0f
#define ENC_SEARCH_MAX     384

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float dist2d(const float a[2], const float b[2]) {
	const float dx = a[0] - b[0], dy = a[1] - b[1];
	return sqrtf(dx * dx + dy * dy);
}

static float halfW(const CarSim* s) {
	return s->chassis.width > 0.5f ? s->chassis.width * 0.5f : 0.9f;
}

// Curvature (0x801FA46C), as aidriver.c's: 1/R of the circle through three
// points, signed by the bend.
static float curvature(const float a[2], const float b[2], const float c[2]) {
	const float abx = b[0] - a[0], aby = b[1] - a[1];
	const float bcx = c[0] - b[0], bcy = c[1] - b[1];
	const float cax = a[0] - c[0], cay = a[1] - c[1];
	const float den = sqrtf(abx * abx + aby * aby) * sqrtf(bcx * bcx + bcy * bcy) *
		sqrtf(cax * cax + cay * cay);
	if (den < 1e-6f) return 0.0f;
	const float cross = abx * bcy - aby * bcx;
	if (fabsf(cross) < 1e-6f) return 0.0f;
	float r = den / (2.0f * fabsf(cross));
	if (r < 0.001f) r = 0.001f;
	return (cross < 0.0f ? -1.0f : 1.0f) / r;
}

// GetSpeedLimitForCurvature (0x801FA2E4).
static float cornerLimit(float grip, float curv, float top) {
	const float a = grip * 9.8f;
	float c = fabsf(curv);
	const float cMin = a / (top * top);
	if (c < cMin) c = cMin;
	return sqrtf(a / c);
}

static float speedGraph(float d) {
	if (d <= kSpeedGraph[0][0]) return kSpeedGraph[0][1];
	for (int k = 1; k < 5; k++) {
		if (d < kSpeedGraph[k][0]) {
			const float t = (d - kSpeedGraph[k - 1][0]) / (kSpeedGraph[k][0] - kSpeedGraph[k - 1][0]);
			return kSpeedGraph[k - 1][1] + (kSpeedGraph[k][1] - kSpeedGraph[k - 1][1]) * t;
		}
	}
	return kSpeedGraph[4][1];
}

static void playerXY(const CarSim* p, float out[2]) {
	out[0] = p->rbPos[0];
	out[1] = p->rbPos[1];
}

// ------------------------------------------------------------ the line

// The racing line at route point `p` driven the way `fwd` says -- line 0 the
// route's way, line 1 against it (MakeDrivingLineNumberBackwards) -- and the
// road's edges there, metres right of the line.
static void linePoint(int p, bool fwd, EncounterNode* nd) {
	float f[2];
	gps_point_fwd(p, f);
	gps_point_pos(p, nd->pos);
	const float off = gps_line_offset(p, fwd ? 0 : 1);   // right of the route's own way
	nd->pos[0] += off * f[1];
	nd->pos[1] -= off * f[0];
	if (fwd) {
		nd->dir[0] = f[0];
		nd->dir[1] = f[1];
	} else {
		gps_point_back(p, nd->dir);
	}
	float l, r;
	gps_point_halves(p, &l, &r);
	const float offT = fwd ? off : -off;
	nd->lo = (fwd ? -l : -r) - offT;
	nd->hi = (fwd ? r : l) - offT;
}

static float encRandom(u32* s) {
	*s = *s * 1664525u + 1013904223u;
	return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

// DecideSignpostAsGetAwayCar for sign `bits` to `to`, the leader `i`: one it
// must take (IsAlways, type 2) yes; 5, 10, 7, a new route with bit 10 and a
// sign with bit 20 no; otherwise World::Random(1) under 0.75 while the leader
// is within 150 m of racer 1's car (the player's), 0.1 beyond -- a type 4
// sign with bit 26 a third or a half by bit 27. PORT: the port's walk runs
// ENC_AHEAD_M ahead of the car, so a junction is decided by where the two
// cars are when the walk gets to it, which can be up to that far before the
// leader does; retail's FuturePoints are fewer (not decoded).
static bool wantGetaway(Encounter* e, int i, u32 bits, int to) {
	const u32 type = bits & 15u;
	if (type == 2) return true;
	if (type == 5 || type == 10) return false;
	const int rt = gps_point_route(to);
	if (rt >= 0 && ((gps_route_bits(rt) >> 10) & 1u)) return false;
	if (type == 7) return false;
	if ((bits >> 20) & 1u) return false;
	const float me[2] = { e->f->car[i].sim.rbPos[0], e->f->car[i].sim.rbPos[1] };
	float p = dist2d(me, e->playerXY) < ENC_TAKE_NEAR_M ? ENC_TAKE_NEAR : ENC_TAKE_FAR;
	if (((bits >> 26) & 1u) && type == 4)
		p = ((bits >> 27) & 1u) ? ENC_TAKE_4_SET : ENC_TAKE_4_CLEAR;
	return encRandom(&e->rng) < p;
}

// ChaseCar's path: from where the path's walk is, its next state.
static bool followPath(Encounter* e, int* p, bool* fwd) {
	for (int k = e->pathAt; k + 1 < e->pathN; k++) {
		if (e->pathPt[k] != *p || e->pathFwd[k] != *fwd)
			continue;
		e->pathAt = k + 1;
		*p = e->pathPt[k + 1];
		*fwd = e->pathFwd[k + 1];
		return true;
	}
	return false;
}

// FuturePoints::AdvancePoint: the signposts at `*p` that apply to the way the
// car is going (bit 18 against it); a dead end (sign type 3) ends the line;
// the goal's WantToTakeSignpost takes one, else the route's next point.
// AIGoalOffsetPursuit and the ChaseCar wants only what it must (IsAlways,
// sign type 2); GetAwayFromCar decides as above; ChaseCar follows its path
// while it has one. PORT: at a route's end with nothing it must take, the
// first signpost that applies and is not a don't-take (5, 10) -- retail's
// data ends routes with a type-2 sign. DecideSignpostAsCop, the ChaseCar's
// own rule past its path, is not decoded (the racer's here). `e` is NULL for
// DbgFreeRoamPilot's walk.
static bool advance(Encounter* e, int i, int goal, int* p, bool* fwd) {
	if (e && goal == ENC_GOAL_CHASE && e->pathN > 0 && followPath(e, p, fwd))
		return true;
	const int n = gps_point_num_signs(*p);
	int fb = -1;
	bool fbFwd = false;
	for (int k = 0; k < n; k++) {
		const int s = gps_point_sign(*p, k);
		const u32 bits = gps_sign_bits(s);
		if (((bits >> 18) & 1u) == (u32)(*fwd ? 1 : 0))
			continue;
		const u32 type = bits & 15u;
		if (type == 3)
			return false;
		const int to = gps_sign_to(s);
		const bool newFwd = (((bits >> 23) & 1u) != 0) != *fwd;
		if (to < 0)
			continue;
		const bool take = e && goal == ENC_GOAL_GETAWAY ? wantGetaway(e, i, bits, to) : type == 2;
		if (take) {
			*p = to;
			*fwd = newFwd;
			return true;
		}
		if (fb < 0 && type != 5 && type != 10) {
			fb = to;
			fbFwd = newFwd;
		}
	}
	const int next = gps_point_next(*p, *fwd);
	if (next >= 0) {
		*p = next;
		return true;
	}
	if (fb >= 0) {
		*p = fb;
		*fwd = fbFwd;
		return true;
	}
	return false;
}

static void pushNode(EncounterRacer* r, int p, bool fwd) {
	if (r->nNodes == ENCOUNTER_NODES) {
		const int drop = r->cur - 1;
		if (drop <= 0)
			return;
		memmove(&r->node[0], &r->node[drop], sizeof(r->node[0]) * (size_t)(r->nNodes - drop));
		r->nNodes -= drop;
		r->cur -= drop;
	}
	EncounterNode nd;
	nd.point = p;
	nd.fwd = fwd;
	linePoint(p, fwd, &nd);
	nd.v = -1.0f;
	nd.s = 0.0f;
	if (r->nNodes > 0) {
		const EncounterNode* last = &r->node[r->nNodes - 1];
		const float d = dist2d(last->pos, nd.pos);
		if (d < 0.5f)
			return;
		nd.s = last->s + d;
	}
	r->node[r->nNodes++] = nd;
}

// The segment `s` lies on: node k to k + 1.
static int segAt(const EncounterRacer* r, float s) {
	const int n = r->nNodes;
	if (n < 2) return 0;
	int k = r->cur > 0 ? r->cur - 1 : 0;
	if (k > n - 2) k = n - 2;
	while (k > 0 && r->node[k].s > s) k--;
	while (k + 1 < n - 1 && r->node[k + 1].s <= s) k++;
	return k;
}

// The line `s` metres along the walk: where it is, the way it runs and the
// road's edges.
static void sampleAt(const EncounterRacer* r, float s, float pos[3], float dir[2],
		float* lo, float* hi) {
	const int n = r->nNodes;
	if (n == 0) {
		memset(pos, 0, sizeof(float) * 3);
		if (dir) { dir[0] = 1.0f; dir[1] = 0.0f; }
		if (lo) *lo = -3.0f;
		if (hi) *hi = 3.0f;
		return;
	}
	if (n == 1) {
		memcpy(pos, r->node[0].pos, sizeof(float) * 3);
		if (dir) { dir[0] = r->node[0].dir[0]; dir[1] = r->node[0].dir[1]; }
		if (lo) *lo = r->node[0].lo;
		if (hi) *hi = r->node[0].hi;
		return;
	}
	const int k = segAt(r, s);
	const EncounterNode* a = &r->node[k];
	const EncounterNode* b = &r->node[k + 1];
	const float len = b->s - a->s;
	float d[2] = { b->pos[0] - a->pos[0], b->pos[1] - a->pos[1] };
	const float dl = sqrtf(d[0] * d[0] + d[1] * d[1]);
	if (dl > 1e-4f) { d[0] /= dl; d[1] /= dl; } else { d[0] = a->dir[0]; d[1] = a->dir[1]; }
	float t = len > 1e-4f ? (s - a->s) / len : 0.0f;
	if (t < 0.0f) t = 0.0f;
	const float tc = t > 1.0f ? 1.0f : t;
	for (int c = 0; c < 2; c++)
		pos[c] = a->pos[c] + (b->pos[c] - a->pos[c]) * tc + (t > 1.0f ? d[c] * (s - b->s) : 0.0f);
	pos[2] = a->pos[2] + (b->pos[2] - a->pos[2]) * tc;
	if (dir) { dir[0] = d[0]; dir[1] = d[1]; }
	if (lo) *lo = a->lo + (b->lo - a->lo) * tc;
	if (hi) *hi = a->hi + (b->hi - a->hi) * tc;
}

// The walk run on to ENC_AHEAD_M past the car, its signposts taken by `goal`,
// and the cornering limit of every node with the curvature span walked
// either side of it (the last node of a line that ends is a stop).
static void walkExtend(Encounter* e, int i, int goal, EncounterRacer* r) {
	for (int guard = 0; guard < 96 && !r->ended; guard++) {
		if (r->nNodes > 0 && r->node[r->nNodes - 1].s >= r->progress + ENC_AHEAD_M)
			break;
		if (r->nNodes == ENCOUNTER_NODES && r->cur <= 1)
			break;
		int p = r->walkPoint;
		bool f = r->walkFwd;
		if (!advance(e, i, goal, &p, &f)) {
			r->ended = true;
			break;
		}
		r->walkPoint = p;
		r->walkFwd = f;
		pushNode(r, p, f);
	}
	const int n = r->nNodes;
	if (n == 0) return;
	const float last = r->node[n - 1].s;
	for (int k = 0; k < n; k++) {
		EncounterNode* nd = &r->node[k];
		if (nd->v >= 0.0f) continue;
		if (!r->ended && nd->s + ENC_CURV_SPAN > last) break;
		float a[3], b[3], c[3];
		sampleAt(r, nd->s - ENC_CURV_SPAN, a, NULL, NULL, NULL);
		sampleAt(r, nd->s, b, NULL, NULL, NULL);
		sampleAt(r, nd->s + ENC_CURV_SPAN, c, NULL, NULL, NULL);
		nd->v = cornerLimit(e->grip[i], curvature(a, b, c), e->carTop[i]);
	}
	if (r->ended)
		r->node[n - 1].v = 0.0f;
}

// The walk cut back to node `last` and walked on from it afresh -- a new
// goal's signposts, or a new path -- the corner limits near the cut planned
// again.
static void cutWalk(EncounterRacer* r, int last) {
	if (last < 0 || last >= r->nNodes)
		return;
	r->nNodes = last + 1;
	r->walkPoint = r->node[last].point;
	r->walkFwd = r->node[last].fwd;
	r->ended = false;
	const float end = r->node[last].s;
	for (int k = 0; k <= last; k++)
		if (r->node[k].s + ENC_CURV_SPAN > end)
			r->node[k].v = -1.0f;
}

// InterpolateDrivingLineSpeed at `x`: the line's limit there, the corners
// ahead braked for at the class's deceleration (ScrubSpeeds).
static float lineSpeedAt(const Encounter* e, int i, const EncounterRacer* r, float x) {
	const int n = r->nNodes;
	float v = e->carTop[i];
	if (n == 0) return 0.0f;
	if (r->ended && x >= r->node[n - 1].s) return 0.0f;
	for (int k = segAt(r, x); k < n; k++) {
		const EncounterNode* nd = &r->node[k];
		if (nd->v < 0.0f) break;
		float d = nd->s - x;
		if (d < 0.0f) d = 0.0f;
		const float cap = sqrtf(nd->v * nd->v + 2.0f * e->brake[i] * d);
		if (cap < v) v = cap;
		if (d > ENC_AHEAD_M) break;
	}
	return v;
}

// Where the car at `xy` is on its line: `progress` and `lat`, the node it is
// past moved on. The nearest point on the segments from the one before the
// car's to ENC_TRACK_AHEAD past it, the later segment on a tie -- a car on
// the outside of a corner, past the end of one segment and short of the next
// one's start, is at the corner's node, not stuck behind it.
#define ENC_TRACK_AHEAD 4
static void track(EncounterRacer* r, const float xy[2]) {
	const int n = r->nNodes;
	if (n < 2) return;
	int k0 = r->cur - 1, k1 = r->cur + ENC_TRACK_AHEAD;
	if (k0 > n - 2) k0 = n - 2;
	if (k0 < 0) k0 = 0;
	if (k1 > n - 2) k1 = n - 2;
	int best = -1;
	float bestD2 = 0.0f, bestT = 0.0f, bestDir[2] = { 1.0f, 0.0f };
	for (int k = k0; k <= k1; k++) {
		const EncounterNode* a = &r->node[k];
		const EncounterNode* b = &r->node[k + 1];
		float d[2] = { b->pos[0] - a->pos[0], b->pos[1] - a->pos[1] };
		const float len = sqrtf(d[0] * d[0] + d[1] * d[1]);
		if (len > 1e-4f) { d[0] /= len; d[1] /= len; } else { d[0] = a->dir[0]; d[1] = a->dir[1]; }
		// Past the last node the line runs on straight (sampleAt does the same).
		const float t = clampf((xy[0] - a->pos[0]) * d[0] + (xy[1] - a->pos[1]) * d[1], 0.0f,
			k == n - 2 ? len + 50.0f : len);
		const float ex = xy[0] - (a->pos[0] + d[0] * t), ey = xy[1] - (a->pos[1] + d[1] * t);
		const float d2 = ex * ex + ey * ey;
		if (best < 0 || d2 <= bestD2 + 1e-3f) {
			best = k;
			bestD2 = d2;
			bestT = t;
			bestDir[0] = d[0];
			bestDir[1] = d[1];
		}
	}
	r->cur = best;
	const EncounterNode* a = &r->node[best];
	r->progress = a->s + bestT;
	const float px = a->pos[0] + bestDir[0] * bestT, py = a->pos[1] + bestDir[1] * bestT;
	r->lat = (xy[0] - px) * bestDir[1] - (xy[1] - py) * bestDir[0];
}

// The off-page mover's node: past every node it has driven by.
static void trackAlong(EncounterRacer* r) {
	while (r->cur + 1 < r->nNodes - 1 && r->node[r->cur + 1].s <= r->progress)
		r->cur++;
}

// --------------------------------------------------------- the mover

static void setMover(Encounter* e, Opponent* o, bool mellow) {
	carsim_set_mellow(&o->sim, mellow, o->perf, e->f->mellowMu);
}

// Car::Deactivate: off the road, not drawn, not collided with.
static void deactivate(Encounter* e, int i, const char* why, void (*log)(const char* msg)) {
	Opponent* o = &e->f->car[i];
	if (o->inactive) return;
	o->inactive = true;
	e->r[i].phys = false;
	e->deactivated++;
	if (log) {
		char b[160];
		snprintf(b, sizeof(b), "encounter: racer %d (%s) off the road -- %s, %.0f m from the "
			"player", i, o->name, why, e->r[i].playerDist);
		log(b);
	}
}

// The off-page pose: the line at `progress`, `lat` right of it.
static void posePoint(const EncounterRacer* r, float pos[3], float* yaw) {
	float dir[2];
	sampleAt(r, r->progress, pos, dir, NULL, NULL);
	pos[0] += r->lat * dir[1];
	pos[1] -= r->lat * dir[0];
	*yaw = atan2f(dir[1], dir[0]);
}

// TooCloseToAnotherCar(bVector2, 30, 300, self) (0x8002F844's two-distance
// form): the player and the traffic on the road within 30 m, the other racer
// within 300.
static bool tooClose(const Encounter* e, const EncounterFrame* fr, int self, const float pos[3]) {
	float pp[2];
	playerXY(fr->player, pp);
	if (dist2d(pp, pos) < ENC_CLEAR_CAR_M) return true;
	for (int j = 0; j < e->f->count && j < ENCOUNTER_CARS; j++) {
		if (j == self || !encounter_active(e, j)) continue;
		const float q[2] = { e->f->car[j].sim.rbPos[0], e->f->car[j].sim.rbPos[1] };
		if (dist2d(q, pos) < ENC_CLEAR_RACER_M) return true;
	}
	const Traffic* t = fr->traffic;
	if (t && t->open) {
		for (int k = 0; k < t->count; k++) {
			const TrafficCar* c = &t->car[k];
			if (!c->used || !c->active || c->stopped) continue;
			const float q[2] = { c->x, c->y };
			if (dist2d(q, pos) < ENC_CLEAR_CAR_M) return true;
		}
	}
	return false;
}

// TryToPlaceInactiveCars, the placing: Activate, then ResetToRoutePoint on
// the racing line the way the player is going. PORT: the line's speed byte
// (100 m/s throughout RoutesFreeRoam.bin) is not the reset speed; the car
// starts at the speed its goal wants there.
static void placeCar(Encounter* e, const EncounterFrame* fr, int i, int p, bool fwd) {
	Opponent* o = &e->f->car[i];
	EncounterRacer* r = &e->r[i];
	memset(r, 0, sizeof(*r));
	// Activate: not poofed, not blinking; the engage flag cleared (+0x11CC);
	// the free-roam goal.
	e->poofed[i] = false;
	o->blinkSec = 0.0f;
	e->engageFlag[i] = false;
	e->goal[i] = ENC_GOAL_PURSUIT;
	r->walkPoint = p;
	r->walkFwd = fwd;
	r->sinceUnstuck = 1e6f;
	// A fresh DrivingChoice: the line, at the slowest lateral speed.
	o->latRate = OPPONENT_AVOID_RATE_MIN;
	o->choiceSec = OPPONENT_AVOID_HOLD_SEC;
	o->avoiding = false;
	o->accident = o->accidentReset = false;
	o->hitBy = 0;
	r->hitWho = -2;
	o->fwdAccel = 0.0f;
	pushNode(r, p, fwd);
	walkExtend(e, i, e->goal[i], r);
	float pos[3], yaw;
	posePoint(r, pos, &yaw);
	float pp[2];
	playerXY(fr->player, pp);
	r->playerDist = dist2d(pp, pos);
	const float floor = r->playerDist <= ENC_FLOOR_NEAR_M ? ENC_FLOOR_NEAR : ENC_FLOOR_FAR;
	float v = fmaxf(speedGraph(r->playerDist) * fr->player->speed, floor);
	v = fminf(v, lineSpeedAt(e, i, r, 0.0f) * o->skillScale);
	r->speed = v;
	o->inactive = false;
	carsim_set_mellow(&o->sim, false, 0.0f, 0.0f);
	carsim_place(&o->sim, NULL, pos, yaw);
	carsim_traffic_pose(&o->sim, NULL, pos[0], pos[1], pos[2], yaw, r->speed, 0.0f);
	aidriver_zero_memory(&o->drv);
	o->physSec = 0.0f;
	o->lookahead = 0.0f;
	o->lookaheadSec = 0.0f;
	e->placedAt[i] = e->clock;
	e->mode[i] = 1;
	e->placed++;
	if (fr->log) {
		char b[200];
		snprintf(b, sizeof(b), "encounter: racer %d (%s) placed at route point %d %s, "
			"(%.0f, %.0f), %.0f m from the player, rolling %.1f m/s", i, o->name, p,
			fwd ? "fwd" : "back", pos[0], pos[1], r->playerDist, r->speed);
		fr->log(b);
	}
}

static void tryToPlaceInactiveCars(Encounter* e, const EncounterFrame* fr) {
	float last = -1e9f;
	for (int i = 0; i < ENCOUNTER_CARS; i++)
		if (e->placedAt[i] > last) last = e->placedAt[i];
	if (!(e->clock - last > ENC_PLACE_GAP_SEC))
		return;
	float pp[2];
	playerXY(fr->player, pp);
	const float minM = e->dbgPlaceM > 0.0f ? e->dbgPlaceM : ENC_PLACE_MIN_M;
	// One failed search a 10 s window spelt out in the log.
	char tr[400], b[160];
	int trn = 0;
	const bool trace = fr->log && e->clock - e->traceAt >= 10.0f;
	tr[0] = '\0';
	for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS; i++) {
		const Opponent* o = &e->f->car[i];
		if (!o->simReady || !o->inactive) continue;
		bool fwd;
		int p = traffic_route_point(fr->player, &fwd);
		const int p0 = p;
		const bool f0 = fwd;
		if (p < 0 || !traffic_guess_target(&e->rng, false, &p, &fwd, minM)) {
			if (trace) {
				e->traceAt = e->clock;
				snprintf(b, sizeof(b), "encounter: no spot -- the player's route point %d %s, "
					"%s", p0, f0 ? "fwd" : "back", p0 < 0 ? "none" : "no target 250 m on");
				fr->log(b);
			}
			continue;
		}
		if (trace)
			trn = snprintf(tr, sizeof(tr), "encounter: no spot -- from %d %s:", p0,
				f0 ? "fwd" : "back");
		float walked = minM;
		int prev = -1;
		while (p != prev) {
			float pos[3];
			gps_point_pos(p, pos);
			// GetPostedForRacersForward: bit 11, whichever the way.
			const float d = dist2d(pp, pos);
			const bool posted = ((gps_point_bits(p) >> 11) & 1u) != 0;
			const bool close = d >= minM && posted && tooClose(e, fr, i, pos);
			if (d >= minM && posted && !close) {
				placeCar(e, fr, i, p, fwd);
				return;
			}
			if (trace && trn >= 0 && trn < (int)sizeof(tr))
				trn += snprintf(tr + trn, sizeof(tr) - (size_t)trn, " %d %.0fm%s%s", p, d,
					posted ? "" : " unposted", close ? " close" : "");
			walked += ENC_PLACE_STEP_M;
			prev = p;
			if (!traffic_guess_target(&e->rng, false, &p, &fwd, ENC_PLACE_STEP_M)) {
				if (trace && trn >= 0 && trn < (int)sizeof(tr))
					trn += snprintf(tr + trn, sizeof(tr) - (size_t)trn, " (no target on)");
				break;
			}
			if (walked >= ENC_PLACE_MAX_M)
				break;
		}
		if (trace) {
			e->traceAt = e->clock;
			fr->log(tr);
		}
		e->placeFails++;
	}
}

// MakeOpponentsDriveNearPlayer: each active car's side of the player, the
// lateral offset its SetOffsetPursuit takes.
static void makeOpponentsDriveNearPlayer(Encounter* e) {
	for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS; i++) {
		if (!encounter_active(e, i)) continue;
		EncounterRacer* r = &e->r[i];
		const float near = e->mode[i] == 1 ? ENC_NEAR_AHEAD_M : ENC_NEAR_BESIDE_M;
		if (fabsf(r->playerD) < near) {
			if (e->mode[i] == 1)
				e->mode[i] = r->playerL < 0.0f ? 2 : 3;
			else if (e->mode[i] == 3 && r->playerL < -ENC_SIDE_SWAP_M)
				e->mode[i] = 2;
			else if (e->mode[i] == 2 && r->playerL > ENC_SIDE_SWAP_M)
				e->mode[i] = 3;
			r->offsetL = e->mode[i] == 2 ? -ENC_SIDE_M : ENC_SIDE_M;
		} else {
			e->mode[i] = 1;
			r->offsetL = 0.0f;
		}
	}
}

static void tryAndDeactivateOldOpponents(Encounter* e, void (*log)(const char* msg)) {
	for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS; i++) {
		if (!encounter_active(e, i)) continue;
		if (e->clock - e->placedAt[i] > ENC_OLD_SEC && e->r[i].playerDist >= ENC_OLD_M)
			deactivate(e, i, "placed over 30 s ago", log);
	}
}

// `from` moved `step` toward `to`, and no farther.
static float toward(float from, float to, float step) {
	if (from < to) return from + step < to ? from + step : to;
	return from - step > to ? from - step : to;
}

// Where `xy` is on racer `r`'s line, found as track() finds the car's own:
// the nearest point on the segments from two before the car's to the last
// starting within `reach` metres on, the first one running back as far
// again. Metres along the walk, metres right of the line and its way there.
static bool lineFrame(const EncounterRacer* r, const float xy[2], float reach, float* s,
		float* lat, float dir[2]) {
	const int n = r->nNodes;
	if (n < 2) return false;
	int k0 = r->cur - 2;
	if (k0 > n - 2) k0 = n - 2;
	if (k0 < 0) k0 = 0;
	int best = -1;
	float bestD2 = 0.0f, bestT = 0.0f, bestDir[2] = { 1.0f, 0.0f };
	for (int k = k0; k <= n - 2; k++) {
		const EncounterNode* a = &r->node[k];
		const EncounterNode* b = &r->node[k + 1];
		if (k > k0 && a->s > r->progress + reach) break;
		float d[2] = { b->pos[0] - a->pos[0], b->pos[1] - a->pos[1] };
		const float len = sqrtf(d[0] * d[0] + d[1] * d[1]);
		if (len > 1e-4f) { d[0] /= len; d[1] /= len; } else { d[0] = a->dir[0]; d[1] = a->dir[1]; }
		const float t = clampf((xy[0] - a->pos[0]) * d[0] + (xy[1] - a->pos[1]) * d[1],
			k == k0 ? -reach : 0.0f, k == n - 2 ? len + 50.0f : len);
		const float ex = xy[0] - (a->pos[0] + d[0] * t), ey = xy[1] - (a->pos[1] + d[1] * t);
		const float d2 = ex * ex + ey * ey;
		if (best < 0 || d2 <= bestD2 + 1e-3f) {
			best = k;
			bestD2 = d2;
			bestT = t;
			bestDir[0] = d[0];
			bestDir[1] = d[1];
		}
	}
	if (best < 0) return false;
	const EncounterNode* a = &r->node[best];
	*s = a->s + bestT;
	const float px = a->pos[0] + bestDir[0] * bestT, py = a->pos[1] + bestDir[1] * bestT;
	*lat = (xy[0] - px) * bestDir[1] - (xy[1] - py) * bestDir[0];
	dir[0] = bestDir[0];
	dir[1] = bestDir[1];
	return true;
}

static float halfL(const CarSim* s) {
	return s->chassis.length > 1.0f ? s->chassis.length * 0.5f : 2.25f;
}

// A car round racer `i` by its avoidWho id (another racer's index, -1 the
// player's car, OPPONENT_AVOID_TRAFFIC + k traffic car k): where it is, its
// velocity and its half length and width. False when it is not on the road,
// or is blinking (passed through, as in a race).
static bool encCarPose(const Encounter* e, const EncounterFrame* fr, int id, float xy[2],
		float vel[2], float* hl, float* hw) {
	if (id == -1) {
		const CarSim* p = fr->player;
		if (!p) return false;
		playerXY(p, xy);
		vel[0] = p->vel[0];
		vel[1] = p->vel[1];
		*hl = halfL(p);
		*hw = halfW(p);
		return true;
	}
	if (id >= OPPONENT_AVOID_TRAFFIC) {
		const int k = id - OPPONENT_AVOID_TRAFFIC;
		if (!fr->traffic || !fr->traffic->open || k >= fr->traffic->count) return false;
		const TrafficCar* t = &fr->traffic->car[k];
		if (!t->used || !t->active) return false;
		xy[0] = t->x;
		xy[1] = t->y;
		if (t->mellow) {
			vel[0] = t->sim.vel[0];
			vel[1] = t->sim.vel[1];
		} else {
			vel[0] = cosf(t->heading) * t->speed;
			vel[1] = sinf(t->heading) * t->speed;
		}
		*hl = halfL(&t->sim);
		*hw = halfW(&t->sim);
		return true;
	}
	if (id < 0 || id >= e->f->count || id >= ENCOUNTER_CARS || !encounter_active(e, id))
		return false;
	const Opponent* q = &e->f->car[id];
	if (opponent_blinking(q)) return false;
	const EncounterRacer* rq = &e->r[id];
	if (rq->phys) {
		xy[0] = q->sim.rbPos[0];
		xy[1] = q->sim.rbPos[1];
		vel[0] = q->sim.vel[0];
		vel[1] = q->sim.vel[1];
	} else {
		float p[3], d[2];
		sampleAt(rq, rq->progress, p, d, NULL, NULL);
		xy[0] = p[0] + rq->lat * d[1];
		xy[1] = p[1] - rq->lat * d[0];
		vel[0] = d[0] * rq->speed;
		vel[1] = d[1] * rq->speed;
	}
	*hl = halfL(&q->sim);
	*hw = halfW(&q->sim);
	return true;
}

// Car `id` placed on racer `i`'s line (lineFrame): metres down the line from
// the racer, metres right of the line and its speed down it. False when it
// is not on the road or not within OPPONENT_AVOID_NEAR_M up or down the line
// (NearbyAvoidableList::Build is not decoded, as in opponent.c).
static bool encCarOnLine(const Encounter* e, const EncounterFrame* fr, int i, int id,
		float* relD, float* lat, float* v, float* hl, float* hw) {
	const EncounterRacer* r = &e->r[i];
	float xy[2], vel[2], s, d[2];
	if (!encCarPose(e, fr, id, xy, vel, hl, hw)) return false;
	float here[3];
	sampleAt(r, r->progress, here, NULL, NULL, NULL);
	const float dx = xy[0] - here[0], dy = xy[1] - here[1];
	const float reach = OPPONENT_AVOID_NEAR_M + 20.0f;
	if (dx * dx + dy * dy > reach * reach) return false;
	if (!lineFrame(r, xy, OPPONENT_AVOID_NEAR_M, &s, lat, d)) return false;
	*relD = s - r->progress;
	if (fabsf(*relD) > OPPONENT_AVOID_NEAR_M) return false;
	*v = vel[0] * d[0] + vel[1] * d[1];
	return true;
}

// The cars round racer `i` as its CollisionPredictor sees them, opponent.c's
// buildScene / addAvoidable on the racer's own line. PORT: every car keeps
// its lateral -- the other racers' choices are on lines of their own -- and
// the road either side is the racer's own clamp (`lmin` .. `lmax` there).
static void encScene(OpponentAvoidScene* sc, const Encounter* e, const EncounterFrame* fr,
		int i, float alongV) {
	const Opponent* o = &e->f->car[i];
	const EncounterRacer* r = &e->r[i];
	sc->n = 0;
	sc->minT = 1e9f;
	sc->halfAlong = halfL(&o->sim);
	sc->halfAcross = halfW(&o->sim);
	sc->road = true;
	const float fwd = alongV > OPPONENT_AVOID_PLAN_SPEED ? alongV : OPPONENT_AVOID_PLAN_SPEED;
	for (int k = 0; k < OPPONENT_AVOID_SAMPLES; k++) {
		float p[3], lo, hi;
		sc->d[k] = fwd * opponent_avoid_sample_sec(k);
		sc->sh[k] = 0.0f;
		sampleAt(r, r->progress + sc->d[k], p, NULL, &lo, &hi);
		const float a = lo + sc->halfAcross + 0.3f, b = hi - sc->halfAcross - 0.3f;
		sc->lo[k] = a < b ? a : 0.5f * (lo + hi);
		sc->hi[k] = a < b ? b : 0.5f * (lo + hi);
	}
	const int nTraffic = fr->traffic && fr->traffic->open ? fr->traffic->count : 0;
	for (int k = -ENCOUNTER_CARS - 1; k < nTraffic; k++) {
		// The other racers, then the player's car, then the traffic.
		const int id = k < -1 ? k + ENCOUNTER_CARS + 1 : (k == -1 ? -1 : OPPONENT_AVOID_TRAFFIC + k);
		if (id == i) continue;
		float relD, lat, v, hl, hw;
		if (!encCarOnLine(e, fr, i, id, &relD, &lat, &v, &hl, &hw)) continue;
		const float relV = v - alongV;
		if (fabsf(relV) > OPPONENT_AVOID_BACK_RELV) {
			const float t = relD / -relV;
			if (t > 0.0f && t < sc->minT) sc->minT = t;
		}
		const float pad = id == -1 ? OPPONENT_AVOID_PLAYER_PAD : 0.0f;
		if (relD <= -(sc->halfAlong + hl + pad)) continue;
		if (sc->n >= OPPONENT_AVOID_LIST_MAX) continue;
		OpponentAvoidCar* c = &sc->car[sc->n++];
		c->id = id;
		c->relD = relD;
		c->relL = lat - r->lat;
		c->speed = v;
		c->halfAlong = hl;
		c->halfAcross = hw;
		c->sep = fabsf(lat - r->driveL);
		c->close = fabsf(relD) < sc->halfAlong + hl &&
			c->sep < sc->halfAcross + hw + OPPONENT_AVOID_SIDE_PAD;
		const float run = v > 0.0f ? v : 0.0f;
		for (int s = 0; s < OPPONENT_AVOID_SAMPLES; s++) {
			c->d[s] = relD + run * opponent_avoid_sample_sec(s);
			c->l[s] = lat;
		}
	}
}

// Racer `i`'s choice this frame between its goal's default action and
// AIActionAvoidAvoidable, as opponent.c's driveChoice: the default's choice
// (DriveAtOffset's SetChoice, or JustDrive's TryAndDriveBestLine), unless
// its drive target hits a car within the horizon; then an offset clear of
// it, the side away from it first, or with none AvoidAvoidable's speed.
// Returns that speed, or a figure past any speed when it asks for none.
// `lo` / `hi` are the road's edges at the car, `lmin` / `lmax` the offsets
// it may take there.
// AIActionIJustGotHit (opponent.h's opponent_hit_step): racer `i`'s body
// touched the player's car or the other racer since the last think (`hits`,
// opponent.c's bits). GetMostImportantNearbyCollidingAvoidable takes race cars
// only -- not traffic -- the player's before the other racer. Accomplished the
// first think it touches none. No race runs in free roam, so the floor under
// the player's speed always applies (pCurrentRace null at 0x80011EE0); in the
// getaway from its 10th second, as in a race. Returns the speed it asks, or
// a negative number when the action is not on.
static float encGotHit(Encounter* e, const EncounterFrame* fr, int i, u32 hits,
		const float line[3], const float dir[2], float lo, float hi) {
	Opponent* o = &e->f->car[i];
	EncounterRacer* r = &e->r[i];
	int who = -2;
	float bestD = 0.0f, bestL = 0.0f, bestHl = 0.0f, bestHw = 0.0f;
	for (int k = -1; k < ENCOUNTER_CARS; k++) {
		if (k < 0 ? !(hits & OPPONENT_HIT_PLAYER) || !fr->player
				: k == i || !(hits & (1u << k)) || !encounter_active(e, k))
			continue;
		float d, l, v, hl, hw;
		if (!encCarOnLine(e, fr, i, k, &d, &l, &v, &hl, &hw)) continue;
		bool take = who == -2;
		if (!take && (k < 0) == (who < 0))
			take = (bestD < 0.0f && d > 0.0f) || d < bestD;
		if (take) {
			who = k;
			bestD = d;
			bestL = l;
			bestHl = hl;
			bestHw = hw;
		}
	}
	if (who == -2) {
		r->hitWho = -2;
		return -1.0f;
	}
	if (r->hitWho != who && fr->log) {
		char b[160];
		snprintf(b, sizeof(b), "encounter: racer %d (%s) IJustGotHit by %s, %.1f m ahead, "
			"%.1f m right", i, o->name, who < 0 ? "the player" : e->f->car[who].name, bestD,
			bestL - r->lat);
		fr->log(b);
	}
	r->hitWho = who;
	const int headCls = bestD > 0.0f
		? opponent_heading_class(&o->sim, o->drv.steerTo, line, dir, lo, hi, false) : 0;
	const bool floorOn = !e->ga.on || e->ga.clock > OPPONENT_HIT_FLOOR_SEC;
	return opponent_hit_step(&o->sim, r->lat, halfW(&o->sim), halfL(&o->sim), &r->goalL,
		&o->latRate, bestL, bestHw, bestHl, bestD, headCls, who < 0 ? fr->player : NULL,
		floorOn);
}

static float encAvoid(Encounter* e, const EncounterFrame* fr, int i, float alongV, float speed,
		float lo, float hi, float lmin, float lmax) {
	Opponent* o = &e->f->car[i];
	EncounterRacer* r = &e->r[i];
	if (!o->avoiding && r->action == 1) {
		// DriveAtOffset: the player's L on the line plus the offset.
		const float l = (r->lat - r->playerL) + r->offsetL;
		r->goalL = lmin < lmax ? clampf(l, lmin, lmax) : 0.5f * (lo + hi);
		o->latRate = ENC_OFFSET_RATE;
	}
	OpponentAvoidScene sc;
	encScene(&sc, e, fr, i, alongV);
	int who = -1;
	const float hit = opponent_avoid_hit_time(&sc, r->driveL, r->goalL, o->latRate, &who);
	if (hit > OPPONENT_AVOID_HORIZON) {
		if (o->avoiding) {
			// HasBeenAccomplished: not while its car is still near and either
			// traffic not yet OPPONENT_AVOID_TRAFFIC_PAST behind or the action
			// younger than avoidDone.
			float relD, lat, v, hl, hw;
			if (encCarOnLine(e, fr, i, o->avoidWho, &relD, &lat, &v, &hl, &hw) &&
					((o->avoidWho >= OPPONENT_AVOID_TRAFFIC && relD >= OPPONENT_AVOID_TRAFFIC_PAST) ||
					o->avoidSec < o->avoidDone))
				return 1e9f;
			o->avoiding = false;
		}
		// TryAndDriveBestLine: line 0 at the slowest lateral speed, once no
		// nearby car is within OPPONENT_AVOID_BACK_SEC of meeting this one and
		// the way back is clear. DriveAtOffset's alongside state sets its own
		// choice next frame.
		if (r->action != 1 && r->goalL != 0.0f && sc.minT > OPPONENT_AVOID_BACK_SEC &&
				opponent_avoid_hit_time(&sc, r->driveL, 0.0f, OPPONENT_AVOID_RATE_MIN, NULL) >
				OPPONENT_AVOID_HORIZON) {
			r->goalL = 0.0f;
			o->latRate = OPPONENT_AVOID_RATE_MIN;
			o->choiceSec = 0.0f;
		}
		return 1e9f;
	}
	const OpponentAvoidCar* c = &sc.car[who];
	if (!o->avoiding) {
		o->avoidSec = 0.0f;
		o->avoidDone = hit + OPPONENT_AVOID_DONE_PAD_SEC;
		if (fr->log) {
			char b[160];
			snprintf(b, sizeof(b), "encounter: racer %d (%s) AvoidAvoidable %d, %.1f m ahead, "
				"%.1f m right, hit in %.2f s", i, o->name, c->id, c->relD, c->relL, hit);
			fr->log(b);
		}
	}
	if (!o->avoiding || o->avoidWho != c->id) {
		o->avoiding = true;
		o->avoidWho = c->id;
		o->avoidT = hit;
	}
	if (opponent_avoid_offset(&sc, lo, hi, r->driveL, &r->goalL, &o->latRate, &o->choiceSec,
			c->relL > 0.0f, OPPONENT_AVOID_HORIZON))
		return 1e9f;
	return opponent_avoid_pace(o->avoidT, alongV, o->fwdAccel, speed, c);
}

// RaceCarController::DecideBoostFactor for racer `i` (TASKS.md #255,
// difficulty.c). In free roam (TheRaceParameters +0x14 == 0) it is 0.75 on
// every racer: free roam's racers want three quarters of the speed their
// goals ask for. In the getaway (+0x14 == 7) the leader keeps 1 and an AI
// following the player catches up on the gap (RacingCar +0x24, half as much
// again as in a race), the player's speed ratio and the clock since
// StartGetAway. Car +0x478 takes max(boost, 1).
static float encBoost(Encounter* e, const EncounterFrame* fr, int i) {
	Opponent* o = &e->f->car[i];
	const EncounterRacer* r = &e->r[i];
	o->boostSec += fr->dt;
	DiffBoostIn in;
	memset(&in, 0, sizeof(in));
	in.gameMode = e->ga.on ? 7 : 0;
	in.racer = e->ga.on && i == e->ga.ai;
	in.catchupOn = true;
	in.handicap = 1.0f;
	in.players = 1;
	in.trafficLevel = 3;
	in.raceSec = e->ga.clock;
	in.actionAllows = true;
	in.skill = o->skill;
	in.place = e->ga.playerLeads ? 2 : 1;
	in.gap = e->ga.gap[0];
	in.speed = r->phys ? o->sim.speed : r->speed;
	in.tPlace = e->ga.playerLeads ? 1 : 2;
	in.tGap = e->ga.gap[1];
	in.tSpeed = fr->player ? fr->player->speed : 0.0f;
	in.tRatio = fr->playerRatio;
	o->boost = difficulty_decide_boost(&in, &o->boostSec, o->boost);
	o->sim.catchup = o->boost > 1.0f ? o->boost : 1.0f;
	return o->boost;
}

// AIGoalOutrunSpeedFactorWhenAhead / WhenBehind (SpeedPS 0x80318E1C /
// 0x80318E38), seven floats by career stage: SetDesiredSpeedAsRacer's factor
// in the getaway in place of the skill's, by whether the car leads. Every
// stage's is the same.
#define ENC_OUTRUN_AHEAD  0.84f
#define ENC_OUTRUN_BEHIND 1.0f

// One frame of an active car: AIGoalOffsetPursuit's speed and drive target,
// then the PsychoDriver and its mover -- or the off-page mover.
static void driveCar(Encounter* e, const EncounterFrame* fr, int i) {
	Opponent* o = &e->f->car[i];
	EncounterRacer* r = &e->r[i];
	const CarSim* pl = fr->player;
	const float dt = fr->dt;

	if (r->phys) {
		const float xy[2] = { o->sim.rbPos[0], o->sim.rbPos[1] };
		track(r, xy);
	} else {
		trackAlong(r);
	}
	walkExtend(e, i, e->goal[i], r);

	float line[3], dir[2], lo, hi;
	sampleAt(r, r->progress, line, dir, &lo, &hi);
	float here[2];
	if (r->phys) {
		here[0] = o->sim.rbPos[0];
		here[1] = o->sim.rbPos[1];
	} else {
		here[0] = line[0] + r->lat * dir[1];
		here[1] = line[1] - r->lat * dir[0];
	}
	// GetRelativePositionTo the player: ahead of it and to its right.
	float pp[2];
	playerXY(pl, pp);
	const float rx = here[0] - pp[0], ry = here[1] - pp[1];
	r->playerD = rx * dir[0] + ry * dir[1];
	r->playerL = rx * dir[1] - ry * dir[0];
	r->playerDist = sqrtf(rx * rx + ry * ry);

	// SetDesiredSpeedAsCop: the speed up the line at the re-rolled
	// look-ahead, the driver's skill scale on it.
	o->lookaheadSec += dt;
	if (o->lookaheadSec > difficulty_get()->lookaheadSwitchSec) {
		o->lookaheadSec = 0.0f;
		const float half = difficulty_field(o->skill, DIFF_LOOKAHEAD_JITTER);
		o->lookahead = difficulty_random(&o->rng, half + half) - half;
	}
	// GetSpeedLimit: the graph of the player's speed by distance, floored.
	// GetAwayFromCar's and ChaseCar's are 1e9 (DAT_80318CD8 / CD4), and they
	// set their speed as a racer (SetDesiredSpeedAsRacer): in the getaway
	// (game mode 7) that is the line's limit with no look-ahead (+0x70C is
	// only added in a plain race), times the Outrun factor for leading or
	// following (TASKS.md #255). Both then take +0x778, the catch-up boost.
	const bool pursuit = e->goal[i] == ENC_GOAL_PURSUIT;
	const bool outrun = !pursuit && e->ga.on;
	float at = r->progress + (outrun ? 0.0f : o->lookahead);
	if (at < r->progress) at = r->progress;
	const float scale = !outrun ? o->skillScale
		: (e->ga.playerLeads ? ENC_OUTRUN_BEHIND : ENC_OUTRUN_AHEAD);
	float want = lineSpeedAt(e, i, r, at) * scale * encBoost(e, fr, i);
	if (want > e->carTop[i]) want = e->carTop[i];
	const float floor = r->playerDist <= ENC_FLOOR_NEAR_M ? ENC_FLOOR_NEAR : ENC_FLOOR_FAR;
	if (pursuit)
		want = fminf(want, fmaxf(speedGraph(r->playerDist) * pl->speed, floor));

	// ChangeCurrentL (opponent.c): the drive target walks toward the choice
	// at its lateral rate as the car runs down the road, and is kept on it.
	const float alongV = r->phys ? o->sim.vel[0] * dir[0] + o->sim.vel[1] * dir[1] : r->speed;
	const float hw = halfW(&o->sim);
	const float lmin = lo + hw + 0.3f, lmax = hi - hw - 0.3f;
	r->driveL = toward(r->driveL, r->goalL, o->latRate * (alongV > 0.0f ? alongV * dt : 0.0f));
	r->driveL = lmin < lmax ? clampf(r->driveL, lmin, lmax) : 0.5f * (lo + hi);
	o->choiceSec += dt;
	o->avoidSec += dt;

	// AIActionDriveAtOffset: the error is how far past the 10 m ahead of the
	// player the car is. The getaway's goals choose as racers
	// (ChooseNewActionAsRacer): JustDrive, the best line.
	const float err = r->playerD - ENC_PURSUE_AHEAD_M;
	if (!pursuit || r->playerDist > ENC_AVOIDABLE_M)
		r->action = 0;
	else if (r->action == 0 && fabsf(err) < ENC_ALONG_IN_M)
		r->action = 1;
	else if (r->action == 1 && fabsf(err) > ENC_ALONG_OUT_M)
		r->action = 0;
	if (r->action == 1) {
		float v = pl->speed;
		if (err >= ENC_ALONG_ERR_M) v -= ENC_ALONG_DV;
		else if (err <= -ENC_ALONG_ERR_M) v += ENC_ALONG_DV;
		if (v < ENC_FLOOR_NEAR) v = ENC_FLOOR_NEAR;
		want = fminf(want, v);
	}
	// GetUnstuck goes first (below); then AccidentRecovery on the ground
	// (opponent.h), IJustGotHit (encGotHit) and AvoidAvoidable, by their
	// scores (500, 400, 300); otherwise the default.
	const u32 hits = o->hitBy;
	o->hitBy = 0;
	float hitWant = -1.0f;
	if (r->unstuck) {
		o->avoiding = false;
		o->accident = false;
		r->hitWho = -2;
	} else if (r->phys && opponent_accident_step(o, dt)) {
		if (!o->avoiding && o->accidentSec == 0.0f && fr->log) {
			char b[160];
			snprintf(b, sizeof(b), "encounter: racer %d (%s) AccidentRecovery -- %d wheel(s) down, "
				"up %.2f", i, o->name, o->sim.wheelsDown, o->sim.rot[2][2]);
			fr->log(b);
		}
		o->avoiding = false;
		r->hitWho = -2;
		want = 0.0f;
	} else if (r->phys && (hitWant = encGotHit(e, fr, i, hits, line, dir, lo, hi)) >= 0.0f) {
		o->avoiding = false;
		want = fminf(want, hitWant);
	} else
		want = fminf(want, encAvoid(e, fr, i, alongV, r->phys ? o->sim.speed : r->speed, lo, hi,
			lmin, lmax));
	if (want < 0.0f) want = 0.0f;
	o->desired = want;

	// PORT: a free-roam racer whose line runs out is taken off the road; in
	// the getaway it stops at the end (its last node's limit is 0), and a
	// ChaseCar's next path walks it on.
	if (pursuit && r->ended && r->progress >= r->node[r->nNodes - 1].s - 2.0f) {
		deactivate(e, i, "its line ran out", fr->log);
		return;
	}

	if (!r->phys) {
		// PORT: the off-page mover. Its pedals are a fixed pull and the
		// class's braking, the brake lamps lit while it slows.
		const float dv = clampf(want - r->speed, -e->brake[i] * dt, ENC_FAR_ACCEL * dt);
		r->speed += dv;
		o->fwdAccel = dt > 0.0f ? dv / dt : 0.0f;
		if (r->speed < 0.0f) r->speed = 0.0f;
		o->drv.prevBrake = dv < -0.01f ? 1.0f : 0.0f;
		o->drv.prevThrottle = dv > 0.0f ? 1.0f : 0.0f;
		r->progress += r->speed * dt;
		r->lat = r->driveL;
		trackAlong(r);
		float pos[3], yaw;
		posePoint(r, pos, &yaw);
		carsim_traffic_pose(&o->sim, NULL, pos[0], pos[1], pos[2], yaw, r->speed, dt);
		if (r->playerDist <= ENCOUNTER_PHYS_M && fr->world[i]) {
			// Into the paged-in world: on the ground, rolling, on the
			// MellowMover (Activate's default).
			carsim_place(&o->sim, fr->world[i], pos, yaw);
			carsim_set_rolling(&o->sim, r->speed);
			setMover(e, o, true);
			aidriver_zero_memory(&o->drv);
			aidriver_clear_steering(&o->drv, &o->sim);
			r->phys = true;
			r->stuckSec = 0.0f;
			r->stuckRef = r->progress;
			r->stuckN = r->stuckIdx = 0;
			r->unstuck = false;
			r->sinceReset = 0.0f;
			if (fr->log) {
				char b[160];
				snprintf(b, sizeof(b), "encounter: racer %d (%s) on the ground at (%.0f, %.0f, "
					"%.1f), %.0f m from the player, %.1f m/s", i, o->name, pos[0], pos[1], pos[2],
					r->playerDist, r->speed);
				fr->log(b);
			}
		}
		return;
	}

	// AIActionGetUnstuck (TASKS.md #517): the running one asked whether it
	// is done first, its CleanUp clearing the steering memory; otherwise
	// ChooseNewAction asks for one before anything else. Its goal is the
	// line GetGoalFuturePoint's reach ahead, and the car backs round to face
	// it at OPPONENT_UNSTUCK_DESIRED.
	r->sinceReset += dt;
	r->sinceUnstuck += dt;
	float ugoal[3];
	sampleAt(r, r->progress + OPPONENT_UNSTUCK_GOAL_M, ugoal, NULL, NULL, NULL);
	const int headCls = opponent_heading_class(&o->sim, o->drv.steerTo, line, dir, lo, hi, true);
	if (r->unstuck) {
		r->unstuckSec += dt;
		const bool done = opponent_unstuck_done(&o->sim, ugoal, r->sinceReset, r->unstuckSec);
		const bool failed = opponent_unstuck_failed(&o->sim, headCls, r->unstuckSec);
		if (done || failed || r->unstuckSec > OPPONENT_UNSTUCK_LIFETIME) {
			aidriver_clear_steering(&o->drv, &o->sim);
			r->unstuck = false;
			if (fr->log) {
				char b[160];
				snprintf(b, sizeof(b), "encounter: racer %d (%s) GetUnstuck %s after %.1f s",
					i, o->name, done ? "done" : failed ? "failed" : "timed out", r->unstuckSec);
				fr->log(b);
			}
		}
	} else {
		const float outside = r->lat - hi > lo - r->lat ? r->lat - hi : lo - r->lat;
		if (opponent_unstuck_score(&o->sim, outside, headCls, r->sinceReset, r->sinceUnstuck)) {
			r->unstuck = true;
			r->unstuckSec = 0.0f;
			r->sinceUnstuck = 0.0f;    // JustCalledGetUnstuckAction
			if (fr->log) {
				char b[160];
				snprintf(b, sizeof(b), "encounter: racer %d (%s) GetUnstuck at (%.0f, %.0f), "
					"%.1f m/s, %.1f m outside the road, heading class %d", i, o->name,
					here[0], here[1], o->sim.speed, outside, headCls);
				fr->log(b);
			}
		}
	}

	if (r->unstuck) {
		// CalculatePointsToDrive: the drive target is the goal alone.
		want = OPPONENT_UNSTUCK_DESIRED;
		o->desired = want;
		o->drv.steerTo[0] = ugoal[0];
		o->drv.steerTo[1] = ugoal[1];
		o->drv.lateral_m = 0.0f;
	} else {
		// The drive target: the line at the driver's look-ahead, `driveL`
		// right of it.
		const float bodySpeed = o->sim.vel[0] * o->sim.rot[0][0] + o->sim.vel[1] * o->sim.rot[0][1];
		const float look = aidriver_lookahead(&o->drv, &o->sim, bodySpeed);
		float tgt[3], tdir[2];
		sampleAt(r, r->progress + look, tgt, tdir, NULL, NULL);
		o->drv.steerTo[0] = tgt[0] + r->driveL * tdir[1];
		o->drv.steerTo[1] = tgt[1] - r->driveL * tdir[0];
		o->drv.lateral_m = r->lat - r->driveL;
	}
	o->drv.desired = want;
	o->drv.racing = true;

	// Think's mover: the PhysicsMover while SetToPhysicsMode's timer runs,
	// then the MellowMover once all four wheels are down.
	if (o->physSec > 0.0f) {
		o->physSec -= dt;
		if (o->physSec > 0.0f)
			setMover(e, o, false);
		else if (o->sim.wheelsDown < CARPHYS_WHEELS)
			o->physSec = ENC_PHYS_REARM_SEC;
		else {
			o->physSec = 0.0f;
			setMover(e, o, true);
		}
	}

	CarSimInput in;
	aidriver_drive(&o->drv, &o->sim, dt, &in);
	const float was[3] = { o->sim.vel[0], o->sim.vel[1], o->sim.vel[2] };
	carsim_update(&o->sim, &in, fr->world[i], dt);
	// Car::GetForwardAcceleration, as opponent.c's.
	if (dt > 0.0f) {
		float a = 0.0f;
		for (int k = 0; k < 3; k++) a += (o->sim.vel[k] - was[k]) * o->sim.rot[0][k];
		o->fwdAccel = a / dt;
	}

	// RaceCarController::Think's very-stuck test: the ground made along the
	// line every 1.5 s, and a full ten samples averaging under 0.2 m/s with
	// no player within 50 m is PleaseResetMeLater.
	float stuckAvg = -1.0f;
	r->stuckSec += dt;
	if (r->stuckSec >= OPPONENT_STUCK_SAMPLE_SEC) {
		r->stuckV[r->stuckIdx] = (r->progress - r->stuckRef) / r->stuckSec;
		r->stuckIdx = (r->stuckIdx + 1) % 10;
		if (r->stuckN < 10) r->stuckN++;
		r->stuckSec = 0.0f;
		r->stuckRef = r->progress;
		float sum = 0.0f;
		for (int k = 0; k < r->stuckN; k++) sum += r->stuckV[k];
		float near = 1e9f;
		if (pl) {
			const float px = pl->pos[0] - o->sim.pos[0], py = pl->pos[1] - o->sim.pos[1];
			const float pz = pl->pos[2] - o->sim.pos[2];
			near = sqrtf(px * px + py * py + pz * pz);
		}
		if (!(o->blinkSec > 0.0f) && r->stuckN == 10 && sum / 10.0f < OPPONENT_STUCK_AVG_SPEED &&
				near >= OPPONENT_STUCK_PLAYER_DIST) {
			stuckAvg = sum / 10.0f;
		}
	}
	// PleaseResetMeLater from the very-stuck test, AccidentRecovery or the
	// mover (on its roof, carsim's resetRequest): Car::Move's reset onto the
	// line where the car is, facing down it, still rolling at what it had
	// along the road up to RollingResetSpeed, and blinking.
	{
		const char* why = stuckAvg >= 0.0f ? "very stuck" : o->accidentReset ? "AccidentRecovery" :
			o->sim.resetRequest ? "on its roof" : NULL;
		o->accidentReset = false;
		if (why) {
			float pos[3], yaw;
			r->lat = r->driveL = r->goalL = 0.0f;
			o->avoiding = false;
			posePoint(r, pos, &yaw);
			const float along = o->sim.rot[0][0] * dir[0] + o->sim.rot[0][1] * dir[1];
			float roll = (o->sim.vfwd > 0.0f ? o->sim.vfwd : 0.0f) * (along > 0.0f ? along : 0.0f);
			if (roll > RACE_RESET_SPEED) roll = RACE_RESET_SPEED;
			carsim_place(&o->sim, fr->world[i], pos, yaw);
			carsim_set_rolling(&o->sim, roll);
			// ResetCore: ZeroSteeringMemory, the wheel straight.
			aidriver_zero_memory(&o->drv);
			o->drv.dirState = 1;
			o->sim.resetRequest = false;
			o->blinkSec = OPPONENT_BLINK_SEC;
			r->unstuck = false;
			r->sinceReset = 0.0f;
			r->stuckN = r->stuckIdx = 0;
			r->resets++;
			if (fr->log) {
				char b[192], w[48];
				if (stuckAvg >= 0.0f)
					snprintf(w, sizeof(w), "%s (%.2f m/s over 15 s)", why, stuckAvg);
				else
					snprintf(w, sizeof(w), "%s", why);
				snprintf(b, sizeof(b), "encounter: racer %d (%s) %s -- reset onto its line at "
					"(%.0f, %.0f), %.0f m from the player", i, o->name, w, pos[0], pos[1],
					r->playerDist);
				fr->log(b);
			}
		}
	}
	if (o->sim.rbPos[2] < line[2] - ENC_FALL_M) {
		deactivate(e, i, "fell out of the world", fr->log);
		return;
	}
	if (r->playerDist > ENC_PHYS_OUT_M) {
		// Out of the paged-in world: back on the off-page mover.
		r->speed = o->sim.speed;
		r->lat = r->driveL;
		r->phys = false;
		if (r->unstuck) {
			aidriver_clear_steering(&o->drv, &o->sim);
			r->unstuck = false;
		}
		if (fr->log) {
			char b[160];
			snprintf(b, sizeof(b), "encounter: racer %d (%s) off-page at (%.0f, %.0f), %.0f m from "
				"the player, %.1f m/s", i, o->name, here[0], here[1], r->playerDist, r->speed);
			fr->log(b);
		}
	}
}

// ------------------------------------------------- RandomEncounterState

static bool checkForRightConditions(const Encounter* e, const EncounterCareer* c) {
	if (c->stage >= 6)
		return true;
	bool ok = c->stage != 0 || !c->hasCareerCar;
	if (c->stage == 0 && c->stage0Timeout) ok = false;
	if (c->worldChallenge) ok = false;
	const bool room = ok && (c->outrunWins < c->outrunCap || c->outrunCap == -1);
	return room && e->timer < 0.0f;
}

static bool canTrigger(const Encounter* e, const EncounterCareer* c) {
	if (c->career && c->stage == 0 && c->stage0Timeout)
		return false;
	return e->state == 1;
}

// CanThisCarBeEngaged (0x80127AB8): not the career's stage 0 once its timeout
// is up, and no world challenge on.
static bool canThisCarBeEngaged(const EncounterCareer* c) {
	if (c->career && c->stage == 0 && c->stage0Timeout)
		return false;
	return !c->worldChallenge;
}

// ------------------------------------------------------------ the Outrun

// A car's velocity in xy, its speed (Car +0x440) and its position. The
// off-page mover's is its line's way at its speed.
static void racerMotion(const Encounter* e, int i, float vel[2], float* speed) {
	const Opponent* o = &e->f->car[i];
	const EncounterRacer* r = &e->r[i];
	if (r->phys) {
		vel[0] = o->sim.vel[0];
		vel[1] = o->sim.vel[1];
		*speed = o->sim.speed;
		return;
	}
	float pos[3], dir[2];
	sampleAt(r, r->progress, pos, dir, NULL, NULL);
	vel[0] = dir[0] * r->speed;
	vel[1] = dir[1] * r->speed;
	*speed = r->speed;
}

// The direction GetCarThatCanBeEngaged and DoPathfinding take: the velocity
// in xy, or the forward axis under 1 m/s, normalised.
static void headingOf(const float vel[2], const float rot0[3], float out[2]) {
	out[0] = vel[0];
	out[1] = vel[1];
	if (sqrtf(out[0] * out[0] + out[1] * out[1]) < ENC_DIR_MIN_SPEED) {
		out[0] = rot0[0];
		out[1] = rot0[1];
	}
	const float l = sqrtf(out[0] * out[0] + out[1] * out[1]);
	if (l > 1e-6f) { out[0] /= l; out[1] /= l; } else { out[0] = 1.0f; out[1] = 0.0f; }
}

// GetCarThatCanBeEngaged: the first racer on the road to pass every test,
// -1 for none. A car that passed last time (+0x11CC) gets looser limits --
// -10..40 m, +-15 mph, 20 degrees -- and the loop runs twice. As retail
// writes it the second pass clears the flag the first set (each car's is
// cleared before it is looked at, and none is looked at once one is found),
// so the flags are all clear when it returns and the looser limits are never
// used; the port keeps that.
static int getCarThatCanBeEngaged(Encounter* e, const CarSim* pl) {
	float pd[2];
	const float pv[2] = { pl->vel[0], pl->vel[1] };
	headingOf(pv, pl->rot[0], pd);
	const float pMph = pl->speed * ENC_MPH;
	int found = -1;
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < ENCOUNTER_CARS; i++) {
			const bool was = e->engageFlag[i];
			e->engageFlag[i] = false;
			if (found >= 0 || i >= e->f->count || !encounter_active(e, i))
				continue;
			const CarSim* cs = &e->f->car[i].sim;
			float cv[2], cSpeed, cd[2];
			racerMotion(e, i, cv, &cSpeed);
			headingOf(cv, cs->rot[0], cd);
			const float rel[2] = { cs->rbPos[0] - pl->rbPos[0], cs->rbPos[1] - pl->rbPos[1] };
			if (!(rel[0] * cd[0] + rel[1] * cd[1] > 0.0f))
				continue;
			const float d = sqrtf(rel[0] * rel[0] + rel[1] * rel[1]);
			const float dLo = was ? -ENC_ENGAGE_SLACK : 0.0f;
			const float dHi = was ? ENC_ENGAGE_M + ENC_ENGAGE_SLACK : ENC_ENGAGE_M;
			if (d < dLo || d > dHi)
				continue;
			if (fabsf(cs->rbPos[2] - pl->rbPos[2]) > ENC_ENGAGE_DZ_M)
				continue;
			const float cMph = cSpeed * ENC_MPH, dv = cMph - pMph;
			const float vHi = was ? ENC_ENGAGE_DV_HI + ENC_ENGAGE_DV_SLACK : ENC_ENGAGE_DV_HI;
			const float vLo = was ? ENC_ENGAGE_DV_LO - ENC_ENGAGE_DV_SLACK : ENC_ENGAGE_DV_LO;
			if (cMph < ENC_ENGAGE_MPH || dv < vLo || dv > vHi)
				continue;
			const float dot = clampf(pd[0] * cd[0] + pd[1] * cd[1], -1.0f, 1.0f);
			const float deg = acosf(dot) * (180.0f / 3.14159265f);
			if (!(deg < (was ? ENC_ENGAGE_DEG + ENC_ENGAGE_DEG : ENC_ENGAGE_DEG)))
				continue;
			e->engageFlag[i] = true;
			found = i;
		}
	}
	return found;
}

// TryToEngagePlayer (0x8003D2D8): NotifyPlayerCanTrigger -- state 2 with a
// car that CanThisCarBeEngaged, 3 without -- and Player +0x244 = the car,
// whether or not it can be. PORT: SetSpecialFX(car, 1) sets CarRenderInfo
// +0x2080 on the car, an effect that is not decoded and not drawn here.
static void tryToEngagePlayer(Encounter* e, const EncounterFrame* fr) {
	const int c = getCarThatCanBeEngaged(e, fr->player);
	const bool can = c >= 0 && canThisCarBeEngaged(&fr->career);
	e->state = can ? 2 : 3;
	e->engaged = c;
	const int shown = can ? c : -1;
	if (shown != e->offerLogged && fr->log) {
		char b[200];
		if (shown >= 0) {
			const Opponent* o = &e->f->car[c];
			float cv[2], cSpeed;
			racerMotion(e, c, cv, &cSpeed);
			const float rel[2] = { o->sim.rbPos[0] - fr->player->rbPos[0],
				o->sim.rbPos[1] - fr->player->rbPos[1] };
			snprintf(b, sizeof(b), "encounter: the Outrun is on offer -- racer %d (%s), %.0f m "
				"ahead, %.0f mph, the player %.0f mph", c, o->name,
				sqrtf(rel[0] * rel[0] + rel[1] * rel[1]), cSpeed * ENC_MPH,
				fr->player->speed * ENC_MPH);
		} else {
			snprintf(b, sizeof(b), "encounter: the Outrun is off offer%s",
				c >= 0 ? " (CanThisCarBeEngaged: no)" : "");
		}
		fr->log(b);
		e->offerLogged = shown;
	}
}

// A goal change (SetGetAwayFromCar / SetChaseCar / EngageWinRoadRace): the
// walk past the car's next node is walked again by the new goal. PORT: retail
// deletes the goal and makes a new one; what it leaves of the FuturePoints
// already decided is not decoded.
static void setGoal(Encounter* e, int i, int goal, void (*log)(const char* msg)) {
	if (e->goal[i] == goal)
		return;
	e->goal[i] = goal;
	EncounterRacer* r = &e->r[i];
	cutWalk(r, r->cur + 1 < r->nNodes ? r->cur + 1 : r->nNodes - 1);
	e->pathN = e->pathAt = 0;
	e->chaseSec = 0.0f;
	if (log) {
		char b[128];
		snprintf(b, sizeof(b), "encounter: racer %d (%s) now %s", i, e->f->car[i].name,
			goal == ENC_GOAL_GETAWAY ? "AIGoalGetAwayFromCar (leading)" :
			goal == ENC_GOAL_CHASE ? "AIGoalChaseCar (chasing the player)" : "AIGoalOffsetPursuit");
		log(b);
	}
}

// FindPathToTarget (PORT, see ENC_CHASE_SEARCH_M): the cheapest way along the
// route points from (`p`, `fwd`) to route point `target` within the reach,
// by the moves FuturePoints can make -- the route's next point unless a sign
// it must take applies, and each sign that applies and is not a don't-take
// (5, 10); a dead end (3) has none. Into e->path; false for none.
typedef struct { int p; bool fwd; bool done; float d; int parent; } EncSearch;
static EncSearch s_search[ENC_SEARCH_MAX];

static int searchFind(int n, int p, bool fwd) {
	for (int k = 0; k < n; k++)
		if (s_search[k].p == p && s_search[k].fwd == fwd) return k;
	return -1;
}

static void searchRelax(int* n, int from, int p, bool fwd) {
	float a[3], b[3];
	gps_point_pos(s_search[from].p, a);
	gps_point_pos(p, b);
	const float d = s_search[from].d + dist2d(a, b);
	if (d > ENC_CHASE_SEARCH_M)
		return;
	const int k = searchFind(*n, p, fwd);
	if (k >= 0) {
		if (!s_search[k].done && d < s_search[k].d) {
			s_search[k].d = d;
			s_search[k].parent = from;
		}
		return;
	}
	if (*n >= ENC_SEARCH_MAX)
		return;
	s_search[*n] = (EncSearch){ p, fwd, false, d, from };
	(*n)++;
}

static bool findPath(Encounter* e, int p0, bool f0, int target) {
	int n = 1;
	s_search[0] = (EncSearch){ p0, f0, false, 0.0f, -1 };
	int goal = -1;
	for (;;) {
		int best = -1;
		for (int k = 0; k < n; k++)
			if (!s_search[k].done && (best < 0 || s_search[k].d < s_search[best].d)) best = k;
		if (best < 0)
			break;
		s_search[best].done = true;
		const int p = s_search[best].p;
		const bool fwd = s_search[best].fwd;
		if (p == target) {
			goal = best;
			break;
		}
		const int ns = gps_point_num_signs(p);
		bool must = false, dead = false;
		for (int k = 0; k < ns; k++) {
			const u32 bits = gps_sign_bits(gps_point_sign(p, k));
			if (((bits >> 18) & 1u) == (u32)(fwd ? 1 : 0)) continue;
			if ((bits & 15u) == 3) dead = true;
			if ((bits & 15u) == 2) must = true;
		}
		if (dead)
			continue;
		for (int k = 0; k < ns; k++) {
			const int s = gps_point_sign(p, k);
			const u32 bits = gps_sign_bits(s);
			if (((bits >> 18) & 1u) == (u32)(fwd ? 1 : 0)) continue;
			const u32 type = bits & 15u;
			const int to = gps_sign_to(s);
			if (to < 0 || type == 5 || type == 10) continue;
			searchRelax(&n, best, to, (((bits >> 23) & 1u) != 0) != fwd);
		}
		if (!must) {
			const int next = gps_point_next(p, fwd);
			if (next >= 0)
				searchRelax(&n, best, next, fwd);
		}
	}
	if (goal < 0)
		return false;
	int len = 0;
	for (int k = goal; k >= 0; k = s_search[k].parent) len++;
	// The first ENC_PATH_MAX states from the start.
	int skip = len > ENC_PATH_MAX ? len - ENC_PATH_MAX : 0;
	int at = (len > ENC_PATH_MAX ? ENC_PATH_MAX : len) - 1;
	for (int k = goal; k >= 0; k = s_search[k].parent) {
		if (skip > 0) { skip--; continue; }
		e->pathPt[at] = s_search[k].p;
		e->pathFwd[at] = s_search[k].fwd;
		at--;
	}
	e->pathN = len > ENC_PATH_MAX ? ENC_PATH_MAX : len;
	e->pathAt = 0;
	return true;
}

// AIGoalChaseCar::Think: DoPathfinding to the leader's route point (the
// player's), the walk taken on from the car's next node along it. PORT:
// retail's search runs from the car's position the way it is heading and,
// finding nothing, the other way, turning the car round; the port's walk
// keeps its way and its own signposts when nothing is found.
static void doPathfinding(Encounter* e, const EncounterFrame* fr, int i) {
	EncounterRacer* r = &e->r[i];
	if (r->nNodes == 0)
		return;
	bool tf;
	const int target = traffic_route_point(fr->player, &tf);
	if (target < 0)
		return;
	const int from = r->cur + 1 < r->nNodes ? r->cur + 1 : r->nNodes - 1;
	if (!findPath(e, r->node[from].point, r->node[from].fwd, target)) {
		e->pathN = e->pathAt = 0;
		e->pathFails++;
		return;
	}
	cutWalk(r, from);
	walkExtend(e, i, ENC_GOAL_CHASE, r);
}

// FinishCar for racer `k` (0 the AI, 1 the player): the race time, the place
// and the reason; an AI blinks for 2 s and poofs (BlinkBlinkPoof). In type 7
// outside a Quick Race the player keeps control.
static void finishRacer(Encounter* e, int k, int place, int reason, void (*log)(const char* msg)) {
	EncounterGetaway* g = &e->ga;
	if (g->place[k] != 0)
		return;
	g->place[k] = place;
	g->reason[k] = reason;
	g->time[k] = g->clock;
	if (k == 0) {
		e->f->car[g->ai].blinkSec = ENC_BLINK_SEC;
		e->poofed[g->ai] = true;
	}
	if (log) {
		char b[160];
		snprintf(b, sizeof(b), "encounter: getaway -- %s finished, place %d, %s, %.2f s, gap "
			"%.0f m", k == 0 ? e->f->car[g->ai].name : "the player", place,
			reason == ENC_REASON_WON ? "won (6)" : "lost (7)", g->time[k], g->gap[k]);
		log(b);
	}
}

// StartGetAway: race type 7, the racers the engaged AI and the player, the
// race clock restarted, the manager's BeginRace (state 4); every AI racer
// but the engaged one finished (place 3 on, reason 7), so it blinks and poofs.
static void startGetaway(Encounter* e, const EncounterFrame* fr) {
	EncounterGetaway* g = &e->ga;
	memset(g, 0, sizeof(*g));
	g->on = true;
	g->ai = e->engaged;
	g->dist = fr->metric ? ENC_GETAWAY_M : ENC_GETAWAY_FT_M;
	e->mgr = 4;
	e->resultReady = false;
	int place = 3;
	for (int j = 0; j < e->f->count && j < ENCOUNTER_CARS; j++) {
		if (j == g->ai) continue;
		e->f->car[j].blinkSec = encounter_active(e, j) ? ENC_BLINK_SEC : 0.0f;
		e->poofed[j] = true;
		if (fr->log && encounter_active(e, j)) {
			char b[128];
			snprintf(b, sizeof(b), "encounter: getaway -- racer %d (%s) finished, place %d: blinks "
				"and poofs", j, e->f->car[j].name, place);
			fr->log(b);
		}
		place++;
	}
	if (fr->log) {
		const Opponent* o = &e->f->car[g->ai];
		const float rel[2] = { o->sim.rbPos[0] - fr->player->rbPos[0],
			o->sim.rbPos[1] - fr->player->rbPos[1] };
		char b[200];
		snprintf(b, sizeof(b), "encounter: StartGetAway -- the Outrun against racer %d (%s), %.0f m "
			"ahead; lost at %.1f m behind", g->ai, o->name, sqrtf(rel[0] * rel[0] + rel[1] * rel[1]),
			g->dist);
		fr->log(b);
	}
}

// EndGetAway, at the player's finish: race type 0, the result for main.c's
// NotifyRandomEncounterFinished, Player +0x244 cleared, and the manager's
// EndRace -- MakeOpponentsDriveRandomly, then Abort (state 5).
static void endGetaway(Encounter* e, void (*log)(const char* msg)) {
	EncounterGetaway* g = &e->ga;
	g->on = false;
	e->resultReady = true;
	e->resultWon = g->reason[1] == ENC_REASON_WON;
	e->resultTime = g->time[1];
	e->engaged = -1;
	e->offerLogged = -1;
	for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS; i++)
		if (encounter_active(e, i))
			setGoal(e, i, ENC_GOAL_PURSUIT, NULL);
	e->mgr = 5;
	if (log) {
		char b[128];
		snprintf(b, sizeof(b), "encounter: EndGetAway -- the player %s in %.2f s; clearing the road",
			e->resultWon ? "won" : "lost", e->resultTime);
		log(b);
	}
}

// The racer's position, its speed and its route point with the way it takes
// it (Car::GetRoutePoint; the leader's controller's direction). 0 the AI, 1
// the player.
static void racerState(const Encounter* e, const EncounterFrame* fr, int k, float pos[3],
		float vel[2], float* speed, int* pt, bool* fwd) {
	if (k == 1) {
		const CarSim* pl = fr->player;
		memcpy(pos, pl->rbPos, sizeof(float) * 3);
		vel[0] = pl->vel[0];
		vel[1] = pl->vel[1];
		*speed = pl->speed;
		*pt = traffic_route_point(pl, fwd);
		return;
	}
	const int i = e->ga.ai;
	const EncounterRacer* r = &e->r[i];
	memcpy(pos, e->f->car[i].sim.rbPos, sizeof(float) * 3);
	racerMotion(e, i, vel, speed);
	if (r->nNodes > 0) {
		*pt = r->node[r->cur].point;
		*fwd = r->node[r->cur].fwd;
	} else {
		*pt = -1;
		*fwd = true;
	}
}

static float dist3d(const float a[3], const float b[3]) {
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// One frame of the getaway: RacingCar::UpdateGetAway for each racer still on,
// SortRacingCarsByLeadership, then Race::Update's type 7 code.
static void getawayUpdate(Encounter* e, const EncounterFrame* fr) {
	EncounterGetaway* g = &e->ga;
	g->clock += fr->dt;
	// PORT: an AI the port took off the road mid-race (it fell out of the
	// world) is finished, reason 7, and the race goes on to the player's win.
	if (g->place[0] == 0 && !encounter_active(e, g->ai)) {
		if (fr->log)
			fr->log("encounter: getaway -- the AI is off the road; it is finished");
		finishRacer(e, 0, 2, ENC_REASON_LOST, fr->log);
	}
	const int lead = g->playerLeads ? 1 : 0;
	float lp[3], lv[2], ls;
	int lpt;
	bool lfwd;
	racerState(e, fr, lead, lp, lv, &ls, &lpt, &lfwd);
	for (int k = 0; k < 2; k++) {
		if (g->place[k] != 0) continue;
		if (k == lead) {
			g->gap[k] = 0.0f;
			continue;
		}
		float fp[3], fv[2], fs;
		int fpt;
		bool ffwd;
		racerState(e, fr, k, fp, fv, &fs, &fpt, &ffwd);
		// The straight distance, unless the follower is within 30 m of the
		// leader's route point and not crawling behind a leader that is not.
		g->gap[k] = dist3d(fp, lp);
		if (fs <= ENC_GAP_SLOW && ls > ENC_GAP_SLOW) continue;
		if (lpt < 0) continue;
		float pp[3];
		gps_point_pos(lpt, pp);
		const float d = dist3d(fp, pp);
		if (d >= ENC_GAP_POINT_M) continue;
		if (ls <= ENC_GAP_SLOW) {
			// A stopped leader: a follower going away from the point is
			// ahead of it.
			if ((pp[0] - fp[0]) * fv[0] + (pp[1] - fp[1]) * fv[1] >= 0.0f) continue;
			g->gap[k] = -d;
		} else {
			float dir[2];
			if (lfwd) gps_point_fwd(lpt, dir); else gps_point_back(lpt, dir);
			if (dir[0] * fv[0] + dir[1] * fv[1] <= 0.0f) continue;
			g->gap[k] = ((lp[0] - pp[0]) * dir[0] + (lp[1] - pp[1]) * dir[1]) -
				((fp[0] - pp[0]) * dir[0] + (fp[1] - pp[1]) * dir[1]);
		}
	}
	// ShouldSwapByLeadership (type 7): two racers still on in ascending gap;
	// otherwise by place, so the one still on is first.
	if ((g->place[0] == 0) != (g->place[1] == 0)) {
		g->playerLeads = g->place[1] == 0;
	} else if (g->place[0] == 0) {
		const int fol = 1 - lead;
		if (g->gap[fol] < g->gap[lead]) {
			g->playerLeads = fol == 1;
			if (fr->log) {
				char b[128];
				snprintf(b, sizeof(b), "encounter: getaway -- %s takes the lead (%.1f m)",
					g->playerLeads ? "the player" : e->f->car[g->ai].name, g->gap[fol] - g->gap[lead]);
				fr->log(b);
			}
		}
	}
	// The type 7 code: the leader's gap to 0 and the followers' less it; a
	// follower over GetGetAwayDistance back is finished (reason 7), and with
	// under two left the leader is (reason 6).
	const int L = g->playerLeads ? 1 : 0;
	if (g->place[L] == 0) {
		int left = (g->place[0] == 0) + (g->place[1] == 0);
		const float g0 = g->gap[L];
		g->gap[L] = 0.0f;
		const int F = 1 - L;
		if (g->place[F] == 0) {
			g->gap[F] -= g0;
			if (g->gap[F] > g->dist) {
				finishRacer(e, F, left, ENC_REASON_LOST, fr->log);
				left--;
			}
		}
		if (left < 2)
			finishRacer(e, L, 1, ENC_REASON_WON, fr->log);
	}
	g->logSec -= fr->dt;
	if (g->logSec <= 0.0f && fr->log) {
		g->logSec = 2.0f;
		char b[200];
		snprintf(b, sizeof(b), "encounter: getaway %.1f s -- %s leads, the player's gap %.1f m, "
			"%s's %.1f m, path %d/%d", g->clock, g->playerLeads ? "the player" : "the AI",
			g->gap[1], e->f->car[g->ai].name, g->gap[0], e->pathAt, e->pathN);
		fr->log(b);
	}
	// TheRaceHasFinished, once the player is: EndGetAway.
	if (g->place[1] != 0)
		endGetaway(e, fr->log);
}

// Manager state 4: MakeOpponentBeGetAwayLeader / FollowGetAwayLeader for the
// AI, by where it is in the order, then TryAndDeactivateLostOpponents.
static void tryAndDeactivateLostOpponents(Encounter* e, void (*log)(const char* msg)) {
	for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS; i++)
		if (encounter_active(e, i) && e->poofed[i] && !(e->f->car[i].blinkSec > 0.0f))
			deactivate(e, i, "poofed", log);
}

// TryAndDeactivateAllActiveOpponents: those over 250 m from the player off
// the road; the number left that are poofed or nearer.
static int tryAndDeactivateAllActiveOpponents(Encounter* e, const EncounterFrame* fr) {
	int left = 0;
	float pp[2];
	playerXY(fr->player, pp);
	for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS; i++) {
		if (!encounter_active(e, i)) continue;
		const float q[2] = { e->f->car[i].sim.rbPos[0], e->f->car[i].sim.rbPos[1] };
		if (e->poofed[i] || dist2d(pp, q) < ENC_CLEAR_M)
			left++;
		else
			deactivate(e, i, "the Outrun's clear-up", fr->log);
	}
	return left;
}

// ---------------------------------------------------------------- API

static bool isSuvType(int carType) {
	// opponent.c's kCarsSUV.
	return carType == 8 || carType == 9 || carType == 10;
}

void encounter_open(Encounter* e, OpponentField* f, const CarDb* db, float firstSec,
		void (*log)(const char* msg)) {
	memset(e, 0, sizeof(*e));
	if (!f || f->count <= 0)
		return;
	e->f = f;
	e->on = true;
	e->timer = firstSec >= 0.0f ? firstSec : ENC_INIT_SEC;
	e->rng = f->rngSeed ^ 0x6A09E667u;
	e->engaged = -1;
	e->offerLogged = -1;
	for (int i = 0; i < ENCOUNTER_CARS; i++)
		e->goal[i] = ENC_GOAL_PURSUIT;
	f->mellowMu = carsim_mellow_mu(false, 0u);
	for (int i = 0; i < f->count && i < ENCOUNTER_CARS; i++) {
		Opponent* o = &f->car[i];
		o->inactive = true;
		if (!o->simReady) continue;
		const float lo = db ? db->measureLo : 0.0f, hi = db ? db->measureHi : 10.0f;
		if (!o->classSet)
			o->carClass = aidriver_car_class(o->perf, lo, hi);
		e->carTop[i] = o->sim.topSpeed > 5.0f ? o->sim.topSpeed : 90.0f;
		float grip = aidriver_class_grip(o->carClass);
		// TASKS.md #398: the racer drives on the MellowMover, whose cap is
		// f->mellowMu, not on carsim's own tyres.
		if (f->mellowMu > 0.0f && f->mellowMu < grip) grip = f->mellowMu;
		e->grip[i] = grip;
		e->brake[i] = aidriver_class_braking(o->carClass);
		aidriver_init(&o->drv, o->carClass);
		o->drv.suv = isSuvType(o->carType);
		o->skillScale = difficulty_field(o->skill, DIFF_RACER_SPEED_SCALE);
		o->lookahead = 0.0f;
		o->lookaheadSec = 0.0f;
		o->boost = 1.0f;
		o->boostSec = 1e6f;
		o->sim.catchup = 1.0f;
		o->rng = f->rngSeed ^ (0x9E3779B9u * (u32)(i + 1));
		o->driving = true;
		e->placedAt[i] = -1e6f;
	}
	if (log) {
		char b[160];
		snprintf(b, sizeof(b), "encounter: %d free-roam racer(s) ready, the first chance in "
			"%.0f s", f->count, e->timer);
		log(b);
	}
}

void encounter_close(Encounter* e) {
	if (e) memset(e, 0, sizeof(*e));
}

void encounter_player_contact(Encounter* e, int i) {
	if (!encounter_active(e, i) || !e->r[i].phys) return;
	Opponent* o = &e->f->car[i];
	o->hitBy |= OPPONENT_HIT_PLAYER;
	o->physSec = ENC_PHYS_HOLD_SEC;
	setMover(e, o, false);
}

void encounter_car_contact(Encounter* e, int a, int b) {
	if (a == b || !encounter_active(e, a) || !encounter_active(e, b)) return;
	e->f->car[a].hitBy |= 1u << b;
	e->f->car[b].hitBy |= 1u << a;
}

#define ENC_PILOT_LAT_M 2.0f

bool encounter_debug_pilot(Encounter* e, CarSim* player, const CarSimWorld* w, float speed,
		float dt, void (*log)(const char* msg)) {
	if (!e || !e->on || !player || !(dt > 0.0f) || !(speed > 0.0f))
		return false;
	EncounterRacer* r = &e->pilot;
	const float xy[2] = { player->rbPos[0], player->rbPos[1] };
	if (!e->pilotOn) {
		bool fwd;
		const int p = traffic_route_point(player, &fwd);
		if (p < 0) return false;
		memset(r, 0, sizeof(*r));
		r->walkPoint = p;
		r->walkFwd = fwd;
		pushNode(r, p, fwd);
		walkExtend(e, 0, ENC_GOAL_PURSUIT, r);
		if (r->nNodes < 2) return false;
		track(r, xy);
		e->pilotOn = true;
		e->pilotLogSec = 0.0f;
		if (log) {
			char b[128];
			snprintf(b, sizeof(b), "encounter: DbgFreeRoamPilot on the line from route point %d %s, "
				"%.1f m/s", p, fwd ? "fwd" : "back", speed);
			log(b);
		}
	}
	trackAlong(r);
	walkExtend(e, 0, ENC_GOAL_PURSUIT, r);
	const float v = fminf(speed, lineSpeedAt(e, 0, r, r->progress));
	r->progress += v * dt;
	trackAlong(r);
	if (r->ended && r->progress >= r->node[r->nNodes - 1].s - 2.0f) {
		e->pilotOn = false;   // a fresh walk from where the car is, next frame
		return false;
	}
	float pos[3], dir[2], lo, hi;
	sampleAt(r, r->progress, pos, dir, &lo, &hi);
	const float hw = halfW(player);
	float lat = ENC_PILOT_LAT_M;
	if (lat > hi - hw - 0.3f) lat = hi - hw - 0.3f;
	if (lat < lo + hw + 0.3f) lat = 0.5f * (lo + hi);
	pos[0] += lat * dir[1];
	pos[1] -= lat * dir[0];
	pos[2] = player->pos[2];
	carsim_place(player, w, pos, atan2f(dir[1], dir[0]));
	player->speed = v;
	player->speedAbs = v;
	player->vel[0] = dir[0] * v;
	player->vel[1] = dir[1] * v;
	player->vel[2] = 0.0f;
	e->pilotLogSec -= dt;
	if (log && e->pilotLogSec <= 0.0f) {
		e->pilotLogSec = 5.0f;
		char b[128];
		snprintf(b, sizeof(b), "encounter: DbgFreeRoamPilot at (%.0f, %.0f), %.1f m/s, %.0f m walked",
			pos[0], pos[1], v, r->progress);
		log(b);
	}
	return true;
}

void encounter_traffic_racers(const Encounter* e, TrafficRace* rc) {
	if (!rc) return;
	rc->nRacers = 0;
	if (!e || !e->on || !e->f) return;
	for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS && rc->nRacers < TRAFFIC_RACERS; i++) {
		if (!encounter_active(e, i)) continue;
		rc->racerPos[rc->nRacers][0] = e->f->car[i].sim.rbPos[0];
		rc->racerPos[rc->nRacers][1] = e->f->car[i].sim.rbPos[1];
		rc->racerDist[rc->nRacers] = 0.0f;
		rc->nRacers++;
	}
}

bool encounter_request_getaway(Encounter* e, void (*log)(const char* msg)) {
	if (!encounter_offer_up(e))
		return false;
	e->requested = true;
	if (log) {
		char b[96];
		snprintf(b, sizeof(b), "encounter: RequestGetAway -- racer %d (%s)", e->engaged,
			e->f->car[e->engaged].name);
		log(b);
	}
	return true;
}

float encounter_player_gap(const Encounter* e) {
	if (!e || !e->on || !e->ga.on)
		return 0.0f;
	const EncounterGetaway* g = &e->ga;
	const float d = g->gap[1] - g->gap[0];
	return d < -g->dist ? -g->dist : d;
}

bool encounter_leader_pos(const Encounter* e, float pos[3]) {
	if (!e || !e->on || !e->ga.on || e->ga.playerLeads || !encounter_active(e, e->ga.ai))
		return false;
	memcpy(pos, e->f->car[e->ga.ai].sim.rbPos, sizeof(float) * 3);
	return true;
}

bool encounter_take_result(Encounter* e, bool* won, float* raceSec) {
	if (!e || !e->resultReady)
		return false;
	e->resultReady = false;
	if (won) *won = e->resultWon;
	if (raceSec) *raceSec = e->resultTime;
	return true;
}

void encounter_update(Encounter* e, const EncounterFrame* fr) {
	if (!e || !e->on || !e->f || !fr || !fr->player || !(fr->dt > 0.0f))
		return;
	const float dt = fr->dt;
	e->clock += dt;
	e->playerXY[0] = fr->player->rbPos[0];
	e->playerXY[1] = fr->player->rbPos[1];
	// IsBlinking: StartBlinking's seconds run out whatever the car does.
	for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS; i++) {
		Opponent* o = &e->f->car[i];
		if (o->blinkSec > 0.0f) {
			o->blinkSec -= dt;
			if (o->blinkSec < 0.0f) o->blinkSec = 0.0f;
		}
	}
	// Career free roam is game state 6 with no race on (DAT_803FCA54 == 0):
	// not during the getaway.
	const bool freeRoam = fr->career.freeRoam && !e->ga.on && !e->requested;

	// RandomEncounterState::Tick, in career free roam.
	if (freeRoam) {
		e->timer -= dt;
		if (e->state == 0 && checkForRightConditions(e, &fr->career)) {
			e->state = 1;
			e->raceType = ENC_RACE_OUTRUN;
		}
	}

	// Race::Update: RequestGetAway's StartGetAway, the frame after the press.
	if (e->requested) {
		e->requested = false;
		if (e->mgr == 3 && e->engaged >= 0 && encounter_active(e, e->engaged))
			startGetaway(e, fr);
	}

	switch (e->mgr) {
	case 0:
		// Init: NotifyAboutNewRace.
		e->state = 0;
		e->mgr = 1;
		break;
	case 1:
		// ShouldRandomEncounterHappen.
		if (canTrigger(e, &fr->career)) {
			e->mgr = 3;
			if (fr->log)
				fr->log("encounter: an encounter can happen -- the racers take to the road");
		} else if (!e->condLogged && e->timer < 0.0f && fr->log) {
			e->condLogged = true;
			char b[200];
			snprintf(b, sizeof(b), "encounter: no encounter yet -- stage %d, outruns %d of %d, "
				"career car %d, stage-0 timeout %d, world challenge %d", fr->career.stage,
				fr->career.outrunWins, fr->career.outrunCap, (int)fr->career.hasCareerCar,
				(int)fr->career.stage0Timeout, (int)fr->career.worldChallenge);
			fr->log(b);
		}
		break;
	case 3:
		if (freeRoam) {
			tryToPlaceInactiveCars(e, fr);
			tryToEngagePlayer(e, fr);
			makeOpponentsDriveNearPlayer(e);
			tryAndDeactivateOldOpponents(e, fr->log);
		} else if (e->state == 2) {
			// PORT: the port runs state 3 in free roam only; out of it (a
			// shop, the post-race screen) the offer is down.
			e->state = 3;
			e->engaged = -1;
			e->offerLogged = -1;
		}
		break;
	case 4:
		// The getaway's goals for the AI: the leader's GetAwayFromCar, a
		// follower's ChaseCar, while the race is on.
		if (e->ga.on && encounter_active(e, e->ga.ai)) {
			const int i = e->ga.ai;
			setGoal(e, i, e->ga.playerLeads ? ENC_GOAL_CHASE : ENC_GOAL_GETAWAY, fr->log);
			if (e->goal[i] == ENC_GOAL_CHASE) {
				e->chaseSec -= dt;
				if (e->chaseSec <= 0.0f) {
					e->chaseSec = ENC_CHASE_THINK_SEC;
					doPathfinding(e, fr, i);
				}
			}
		}
		tryAndDeactivateLostOpponents(e, fr->log);
		break;
	case 5:
		// The Outrun's clear-up: the poofed cars once they have blinked, and
		// those far from the player; then RandomEncounterState::Reset (the
		// timer to 0) and state 0.
		tryAndDeactivateLostOpponents(e, fr->log);
		if (tryAndDeactivateAllActiveOpponents(e, fr) == 0) {
			e->timer = ENC_RESET_SEC;
			e->mgr = 0;
			if (fr->log)
				fr->log("encounter: the road is clear -- RandomEncounterState::Reset, manager state 0");
		}
		break;
	}

	for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS; i++)
		if (encounter_active(e, i))
			driveCar(e, fr, i);

	if (e->ga.on)
		getawayUpdate(e, fr);

	// A racer under physics, every 2 s: its pedals cannot be read off a
	// screenshot.
	e->telSec -= dt;
	if (e->telSec <= 0.0f) {
		e->telSec = 2.0f;
		for (int i = 0; fr->log && i < e->f->count && i < ENCOUNTER_CARS; i++) {
			if (!encounter_active(e, i) || !e->r[i].phys) continue;
			const Opponent* o = &e->f->car[i];
			const EncounterRacer* r = &e->r[i];
			char b[220];
			snprintf(b, sizeof(b), "encounter: tel %d (%.0f, %.0f, %.1f) spd %.1f want %.1f boost %.3f "
				"thr %.2f brk %.2f lat %+.1f drive %+.1f goal %+.1f D %.0f L %+.1f %s%s", i,
				o->sim.pos[0], o->sim.pos[1], o->sim.pos[2], o->sim.speed, o->desired, o->boost,
				o->drv.prevThrottle,
				o->drv.prevBrake, r->lat, r->driveL, r->goalL, r->playerD, r->playerL,
				o->sim.mellow ? "mellow" : "physics", r->action ? " alongside" : "");
			fr->log(b);
		}
	}

	e->logSec -= dt;
	if (e->logSec <= 0.0f && fr->log && e->mgr == 3) {
		e->logSec = 10.0f;
		char b[256];
		int n = snprintf(b, sizeof(b), "encounter: placed %d, off %d, no spot %d;", e->placed,
			e->deactivated, e->placeFails);
		for (int i = 0; i < e->f->count && i < ENCOUNTER_CARS; i++) {
			const EncounterRacer* r = &e->r[i];
			if (!encounter_active(e, i)) {
				n += snprintf(b + n, sizeof(b) - (size_t)n, " [%d off]", i);
				continue;
			}
			const Opponent* o = &e->f->car[i];
			n += snprintf(b + n, sizeof(b) - (size_t)n, " [%d %s D %.0f L %.1f %.0f m %.1f/%.1f m/s "
				"mode %d act %d]", i, r->phys ? "phys" : "far", r->playerD, r->playerL,
				r->playerDist, r->phys ? o->sim.speed : r->speed, o->desired, e->mode[i], r->action);
			if (n >= (int)sizeof(b)) break;
		}
		fr->log(b);
	}
}
