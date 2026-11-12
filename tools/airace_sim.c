// Races the AI field offline (TASKS.md #280): the shipping opponent.c,
// aidriver.c, carsim.c, race.c, trackroute.c and topology.c, against the real
// WORLD.NFST collision, on the grid the game puts them on, with no emulator.
// Built and run by tools/airace_sim.py, which documents the usage.
//
// The game's own world queries are compiled in from world.c by marker (see
// the .py), so the collision the cars meet here is the collision they meet on
// the console: world_ground_probe_coord and world_collide_coord over each
// car's own topology coordinate.
//
// What it reports is what a tester sees go wrong: a car that leaves its line,
// runs along a wall, stops, turns round, is reset or never finishes. Every
// incident is printed with where it happened along the route and in the
// world, and one RESULT line per car sums the race up for tools that grep.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cardb.h"
#include "carsim.h"
#include "difficulty.h"
#include "opponent.h"
#include "race.h"
#include "topology.h"
#include "trackdb.h"
#include "trackroute.h"

// ------------------------------------------------- world.c's queries, by marker
//
// The parts of World they read, and nothing else: with a topology loaded
// neither function looks at the drawn sections.
typedef struct {
	Model3D* model;
	unsigned flags;
	float bbmin[3], bbmax[3];
} WorldSection;
typedef struct {
	Topology* topo;
	int count;
	WorldSection* secs;
} World;
#define SECTION_FAR 1u
static bool visHidden(const World* w, int i) { (void)w; (void)i; return false; }
int world_ground_probe_coord(const World* w, int coord, const float* xy, int n,
	float zMax, float* outZ, bool* outHit, float* outN, signed char* outTerrain);
#include "world_collide_info.inc"
#include "world_ground.inc"
bool world_collide_coord(const World* w, int coord, const float from[3], float to[3],
	float yaw, float halfLength, float halfWidth, float zLo, float zHi,
	float outNormal[2], float outArm[2], WorldCollideInfo* info);
#include "world_resolve.inc"

// main.c's body band and footprint, by marker.
#include "world_body.inc"

static World g_world;
static int g_verbose;
static bool g_hit[TOPO_COORD_COUNT];

static void lg(const char* m) { if (g_verbose) printf("  | %s\n", m); }

// main.c's aiProbeThunk / aiCollideThunk, less the profiler.
static int probeThunk(void* ctx, const float* xy, int n, float zMax,
	float* outZ, bool* outHit, float* outN, signed char* outTerrain)
{
	return world_ground_probe_coord(&g_world, (int)(intptr_t)ctx, xy, n, zMax,
		outZ, outHit, outN, outTerrain);
}

static bool collideThunk(void* ctx, const float from[3], float to[3],
	float yaw, float halfLength, float halfWidth, float* outNx, float* outNy,
	float outArm[2])
{
	float nrm[2] = { 0.0f, 0.0f };
	const int coord = (int)(intptr_t)ctx;
	bool hit = world_collide_coord(&g_world, coord, from, to, yaw,
		halfLength * WORLD_FOOTPRINT, halfWidth * WORLD_FOOTPRINT,
		from[2] + WORLD_BODY_LO, from[2] + WORLD_BODY_HI, nrm, outArm, NULL);
	if (hit) {
		g_hit[coord] = true;
		if (outNx) *outNx = nrm[0];
		if (outNy) *outNy = nrm[1];
	}
	return hit;
}

// main.c's aiLineWallsThunk: ailine_build's wall pass through the line's own
// coordinate. AIRACE_NOWALLS=1 in the environment leaves the pass out, to see
// what it changes.
static int g_noWalls;
static int lineWallsThunk(void* ctx, float x, float y, float radius, float zLo,
	float zHi, AiLineWall* out, int max)
{
	(void)ctx;
	static Model3DWall walls[96];
	if (max > 96) max = 96;
	const int n = topo_walls_near(g_world.topo, TOPO_COORD_LINE, x, y, radius, zLo, zHi,
		walls, 0, max);
	for (int i = 0; i < n; i++) {
		out[i].x0 = walls[i].x0;
		out[i].y0 = walls[i].y0;
		out[i].x1 = walls[i].x1;
		out[i].y1 = walls[i].y1;
	}
	return n;
}

// And main.c's aiLineGroundThunk.
static bool lineGroundThunk(void* ctx, float x, float y, float zMax, float* outZ)
{
	(void)ctx;
	const float xy[2] = { x, y };
	bool hit = false;
	return topo_ground_probe(g_world.topo, TOPO_COORD_LINE, xy, 1, zMax, outZ, &hit,
		NULL, NULL) > 0 && hit;
}

// ------------------------------------------------------------- the watching

// How long a condition has to hold before it is an incident, and what counts.
#define WALL_RUN_SEC     1.0f    // in contact with a wall this long
#define OFFLINE_M        5.0f    // this far off its own line...
#define OFFLINE_SEC      2.0f    // ...for this long
#define STALL_SPEED      4.0f    // under this, m/s...
#define STALL_SEC        3.0f    // ...for this long, once racing
#define WRONG_SEC        2.0f    // nose against the route, not backing out
#define AIR_SEC          1.5f    // no wheel down
#define START_GRACE_SEC  6.0f    // the launch is not a stall
#define CLEAR_WARN_M     1.8f    // the line this near a wall: a car on it scrapes

enum { I_WALL, I_OFFLINE, I_STALL, I_WRONG, I_AIR, I_RESET, I_UNSTUCK, I_HOP, I_COUNT };
static const char* kIncName[I_COUNT] = {
	"wall", "offline", "stall", "wrongway", "air", "reset", "unstuck", "hop"
};

typedef struct {
	float t[I_COUNT];        // how long each condition has held
	bool  open[I_COUNT];     // reported, and not over yet
	int   count[I_COUNT];
	float wallSec, offSec, stallSec;
	// time spent touching another car of the field, and how many touches
	float carSec;
	int   carHits;
	bool  prevCar;
	float worstOff;
	float progress;          // metres along the route, unwrapped
	float lastDist;
	float prevSinceReset;
	float prevBlink;
	bool  prevUnstuck;
	bool  finished;
	float finishSec;
	float mellowSec;         // on the MellowMover (opponent.h)
	int   moverSwaps;
	bool  prevMellow;
	float maxSpeed, sumSpeed;
	// when the car first made STALL_SPEED after GO, < 0 until it has: a launch
	// the grid's START_GRACE_SEC hides from the stall count
	float launchSec;
	int   samples;
	// incidents worth printing, at most this many per car
	int   printed;
} Watch;

#define PRINT_MAX 12

static void incident(const Opponent* o, Watch* m, int kind, float sec, const char* extra)
{
	m->count[kind]++;
	if (m->printed >= PRINT_MAX) return;
	m->printed++;
	printf("  INC %-8s %-10s t %6.1f d %7.1f at (%8.1f,%8.1f,%6.1f) spd %5.1f off %+6.1f "
		"str %+5.1f%s%s\n",
		kIncName[kind], o->name, sec, o->coord.dist, o->sim.pos[0], o->sim.pos[1],
		o->sim.pos[2], o->sim.speed, o->drv.lateral_m, o->lastSteerDeg,
		extra ? " " : "", extra ? extra : "");
}

// A condition that has to hold for `need` seconds: counted once per spell.
static void spell(const Opponent* o, Watch* m, int kind, bool on, float need,
	float dt, float sec)
{
	if (!on) {
		m->t[kind] = 0.0f;
		m->open[kind] = false;
		return;
	}
	m->t[kind] += dt;
	if (!m->open[kind] && m->t[kind] >= need) {
		m->open[kind] = true;
		incident(o, m, kind, sec, NULL);
	}
}

int main(int argc, char** argv)
{
	if (argc < 12) {
		fprintf(stderr, "usage: airace_sim pcData worldDir track reverse opponents "
			"seed laps fps playerCar mode verbose [maxSec [trace.csv]]\n");
		return 2;
	}
	const char* pc = argv[1];
	const char* worldDir = argv[2];
	const int track = atoi(argv[3]);
	const bool reverse = atoi(argv[4]) != 0;
	const int nOpp = atoi(argv[5]);
	u32 rng = (u32)strtoul(argv[6], NULL, 0);
	const int laps = atoi(argv[7]);
	const float dt = 1.0f / (float)atof(argv[8]);
	const int playerCar = atoi(argv[9]);
	const int mode = atoi(argv[10]);
	g_verbose = atoi(argv[11]);
	float maxSec = argc > 12 ? (float)atof(argv[12]) : 0.0f;
	// The route, the racing line and every car's path, for plotting.
	FILE* trace = argc > 13 ? fopen(argv[13], "w") : NULL;
	const char* steerDbg = getenv("AIRACE_STEER_DEBUG");
	const char* thrDbg = getenv("AIRACE_THR_DEBUG");
	const char* aiDbg = getenv("AIRACE_AI_DEBUG");
	const char* watchName = getenv("AIRACE_WATCH");
	const float watchFrom = getenv("AIRACE_WATCH_FROM") ? (float)atof(getenv("AIRACE_WATCH_FROM")) : 0.0f;
	const float watchTo = getenv("AIRACE_WATCH_TO") ? (float)atof(getenv("AIRACE_WATCH_TO")) : 1e9f;
	const bool watchWheels = getenv("AIRACE_WATCH_WHEELS") != NULL;

	char path[1024];
	g_noWalls = getenv("AIRACE_NOWALLS") && atoi(getenv("AIRACE_NOWALLS"));
	topo_set_log(lg);
	g_world.topo = topo_open(worldDir);
	if (!g_world.topo) { printf("FAIL no WORLD.NFST in %s\n", worldDir); return 1; }

	snprintf(path, sizeof(path), "%s/GLOBAL/GLOBALB.BUN", pc);
	static CarDb cars;
	if (!cardb_load(path, &cars)) { printf("FAIL no car table\n"); return 1; }
	difficulty_load(path, lg);
	// MatchAICarPerformanceToPlayer's upgrade pass (opponent.h): every AI car
	// fitted to the player's, here driven stock.
	static char bun[1024];
	static PerfDb pdb;
	static RidePhys playerRide;
	static u8 comp[RIDEPHYS_COMPLEAT_SIZE];
	OpponentMatch match;
	memset(&match, 0, sizeof(match));
	match.stageParam = -1.0f;
	snprintf(bun, sizeof(bun), "%s", path);
	if (!getenv("AIRACE_NOMATCH") && perfdb_load(bun, &pdb) &&
			cardb_read_compleat(bun, &cars, playerCar, comp)) {
		ridephys_build(&playerRide, comp, NULL, NULL, NULL);
		match.bunPath = bun;
		match.pdb = &pdb;
		match.player = &playerRide;
	}
	static TrackDb tdb;
	bool p2p = false;
	u32 raceTypeMask = 0;
	if (trackdb_load(path, &tdb)) {
		const TrackRec* tr = trackdb_find(&tdb, (u16)track);
		if (tr) p2p = tr->pointToPoint != 0;
		if (tr) raceTypeMask = tr->raceTypeMask;
	}

	snprintf(path, sizeof(path), "%s/TRACKS/ROUTESL4RA/Paths%d.bin", pc, track);
	RaceRoute* route = race_route_open(path, track, reverse, lg);
	if (!route) { printf("FAIL no route\n"); return 1; }
	snprintf(path, sizeof(path), "%s/TRACKS/ROUTESL4RA/TrackPosMarkersAll.bin", pc);
	RaceMarkers* mk = race_markers_open(path, lg);
	if (!mk) { printf("FAIL no markers\n"); return 1; }

	// world_set_groups(g_world, track, -1, ...) at a race start.
	topo_set_groups(g_world.topo, track, -1);

	static OpponentField f;
	int pslot = 1;
	int built = opponent_build_quickrace(&f, nOpp, &cars, playerCar, 2, &rng, &pslot,
		match.player ? &match : NULL,
		getenv("AIRACE_STARTKIND") ? atoi(getenv("AIRACE_STARTKIND")) : 3 /* SetupCircuit's DetermineStartingPositions */, lg);
	if (built <= 0) { printf("FAIL no field\n"); return 1; }
	float grid[3];
	u16 angle;
	if (!race_grid_slot(mk, track, reverse, pslot, grid, &angle)) {
		printf("FAIL no grid marker %d\n", pslot);
		return 1;
	}
	CarSimWorld fallback = { probeThunk, collideThunk, (void*)(intptr_t)TOPO_COORD_CAR };
	for (int k = 0; k < f.count && k < TOPO_COORD_AI_MAX; k++) {
		const CarSimWorld cw = { probeThunk, collideThunk,
			(void*)(intptr_t)(TOPO_COORD_AI0 + k) };
		f.car[k].world = cw;
	}
	for (int k = 0; k < f.count; k++) {
		float gp[3];
		u16 ga;
		if (!race_grid_slot(mk, track, reverse, f.car[k].slot, gp, &ga)) {
			printf("  no grid marker for slot %d -- %s left off\n", f.car[k].slot,
				f.car[k].name);
			f.car[k].simReady = false;
			continue;
		}
		opponent_place(&f, k, &fallback, gp, (float)ga * (2.0f * (float)M_PI / 65536.0f));
	}
	snprintf(path, sizeof(path), "%s/TRACKS/ROUTESL4RA/Routes%d%s.bin", pc, track,
		reverse ? "B" : "F");
	TrackRouteSet* trs = trackroute_open(path, lg);
	const AiLineWalls walls = { lineWallsThunk, NULL, lineGroundThunk };
	float gridXY[2 * (OPPONENT_MAX + 1)];
	int nGrid = 0;
	gridXY[2 * nGrid] = grid[0];
	gridXY[2 * nGrid++ + 1] = grid[1];
	for (int k = 0; k < f.count; k++) {
		if (!f.car[k].simReady) continue;
		gridXY[2 * nGrid] = f.car[k].sim.pos[0];
		gridXY[2 * nGrid++ + 1] = f.car[k].sim.pos[1];
	}
	f.line = ailine_build(trs, route, grid[2], gridXY, nGrid, g_noWalls ? NULL : &walls,
		g_verbose ? lg : NULL);
	trackroute_close(trs);
	for (int k = 0; k < f.count; k++) f.car[k].sim.controlMode = mode;
	// As main.c's race start: a Quick Race is never Street X.
	f.mellowMu = carsim_mellow_mu(false, raceTypeMask);
	// AIRACE_TYREGRIP=1: plan the profiles on carsim's tyres, as before #398 (bisecting).
	if (getenv("AIRACE_TYREGRIP")) f.physicsMover = true;
	opponent_start_driving(&f, &cars, route, lg);
	if (getenv("AIRACE_TYREGRIP")) f.physicsMover = false;

	const float len = race_route_length(route);
	const bool loops = race_route_loops(route);
	const float goal = loops ? len * (float)laps : len;
	if (!(maxSec > 0.0f)) maxSec = 60.0f + goal / 12.0f;   // 43 km/h average
	printf("RACE %d%s len %.0f m %s laps %d, %d car(s), line %s, p2p flag %d\n",
		track, reverse ? "B" : "F", len, loops ? "loop" : "sprint", loops ? laps : 1,
		f.count, f.line ? "yes" : "NO", (int)p2p);
	for (int k = 0; k < f.count; k++) {
		const Opponent* o = &f.car[k];
		// TireInfo +0x08/+0x0C front and rear, the aero record and the mass.
		const float* cp = o->sim.cpi;
		printf("  CAR %-10s slot %d %s class %d skill %d at (%.1f,%.1f,%.1f) tyre %.2f/%.2f "
			"%.2f/%.2f aero %.2f %.2f %.2f %.2f mass %.0f\n", o->name,
			o->slot, o->carDir, o->carClass, (int)o->skill, o->sim.pos[0], o->sim.pos[1],
			o->sim.pos[2], cp[0xD8 / 4], cp[0xF8 / 4], cp[0xDC / 4], cp[0xFC / 4],
			cp[0x260 / 4], cp[0x264 / 4], cp[0x268 / 4], cp[0x26C / 4], cp[0x110 / 4]);
	}

	if (trace) {
		for (float d = 0.0f; d < len; d += 2.0f) {
			float P[2], L[2];
			AiLineSample a;
			const bool on = ailine_at(f.line, d, &a);
			race_route_sample(route, d, 0.0f, P, NULL);
			if (!ailine_pose(f.line, d, 0.0f, L, NULL))
				race_route_sample(route, d, 0.0f, L, NULL);
			fprintf(trace, "R,%.1f,%.2f,%.2f\nL,%.1f,%.2f,%.2f\n", d, P[0], P[1], d, L[0], L[1]);
			if (on) {
				race_route_sample(route, d, a.left, L, NULL);
				fprintf(trace, "EL,%.1f,%.2f,%.2f\n", d, L[0], L[1]);
				race_route_sample(route, d, a.right, L, NULL);
				fprintf(trace, "ER,%.1f,%.2f,%.2f\n", d, L[0], L[1]);
			}
		}
	}

	// The cornering each car's speed profile plans on the line: v^2 times
	// the line's own curvature through points 7.5 m either side, in g.
	if (getenv("AIRACE_PROFILE_G")) {
		for (int cls = 0; cls < f.count; cls++) {
			if (!f.prof[cls]) continue;
			static float gs[20000];
			int n = 0;
			float worstAt = 0.0f, worst = 0.0f;
			for (float d = 0.0f; d < len && n < 20000; d += 1.0f) {
				float a[2], b[2], c[2];
				if (!ailine_pose(f.line, d - 7.5f, 0.0f, a, NULL) ||
					!ailine_pose(f.line, d, 0.0f, b, NULL) ||
					!ailine_pose(f.line, d + 7.5f, 0.0f, c, NULL)) continue;
				const float ab = hypotf(b[0] - a[0], b[1] - a[1]);
				const float bc = hypotf(c[0] - b[0], c[1] - b[1]);
				const float ca = hypotf(a[0] - c[0], a[1] - c[1]);
				const float cr = fabsf((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]));
				const float k = ab * bc * ca > 1e-6f ? 2.0f * cr / (ab * bc * ca) : 0.0f;
				const float v = aispeed_at(f.prof[cls], d);
				const float g = v * v * k / 9.8f;
				gs[n++] = g;
				if (g > worst) { worst = g; worstAt = d; }
			}
			for (int i = 1; i < n; i++)
				for (int j = i; j > 0 && gs[j - 1] > gs[j]; j--) {
					const float t = gs[j]; gs[j] = gs[j - 1]; gs[j - 1] = t;
				}
			if (n)
				printf("PROFILE car %d planned g p50 %.2f p90 %.2f p98 %.2f max %.2f at %.0f m\n",
					cls, gs[n / 2], gs[n * 9 / 10], gs[n * 98 / 100], worst, worstAt);
		}
	}

	// Each car's speed profile metre by metre between two distances, with
	// the line's world point there.
	if (getenv("AIRACE_PROFILE_FROM")) {
		const float p0 = (float)atof(getenv("AIRACE_PROFILE_FROM"));
		const float p1 = getenv("AIRACE_PROFILE_TO") ? (float)atof(getenv("AIRACE_PROFILE_TO"))
			: p0 + 100.0f;
		for (float d = p0; d <= p1; d += 1.0f) {
			float w[2];
			AiLineSample a;
			if (!ailine_pose(f.line, d, 0.0f, w, NULL) || !ailine_at(f.line, d, &a)) continue;
			float rp[2] = {0.0f, 0.0f};
			race_route_sample(route, d, 0.0f, rp, NULL);
			printf("  prof %.0f at (%.1f,%.1f) rt (%.1f,%.1f) line %+.2f [%+.2f %+.2f]", d, w[0], w[1],
				rp[0], rp[1], a.line, a.left, a.right);
			for (int cls = 0; cls < f.count; cls++)
				if (f.prof[cls]) printf(" c%d %.1f", cls, aispeed_at(f.prof[cls], d));
			printf("\n");
		}
	}

	// Where the line's world points run backwards against the race route: a
	// lateral on the inside of a route corner tighter than it folds the line.
	if (f.line && getenv("AIRACE_FOLDS")) {
		int nFold = 0;
		float foldFrom = -1.0f, worst = 0.0f;
		for (float d = 0.0f; d + 1.0f <= len; d += 1.0f) {
			float a[2], b[2], D[2];
			if (!ailine_pose(f.line, d, 0.0f, a, NULL) || !ailine_pose(f.line, d + 1.0f, 0.0f, b, NULL))
				continue;
			float rp[2];
			race_route_sample(route, d + 0.5f, 0.0f, rp, D);
			const float adv = (b[0] - a[0]) * D[0] + (b[1] - a[1]) * D[1];
			if (adv < 0.0f) {
				if (foldFrom < 0.0f) { foldFrom = d; worst = 0.0f; }
				if (adv < worst) worst = adv;
			} else if (foldFrom >= 0.0f) {
				AiLineSample s;
				ailine_at(f.line, foldFrom, &s);
				printf("  FOLD %.0f-%.0f at (%.1f,%.1f) line %+.1f worst %.2f\n", foldFrom, d, a[0], a[1],
					s.line, worst);
				nFold++;
				foldFrom = -1.0f;
			}
		}
		printf("  FOLDS %d\n", nFold);
	}

	// Every wall within r of x,y in the body's band at height z ("x,y,z,r").
	if (getenv("AIRACE_WALLS_AT")) {
		float x = 0.0f, y = 0.0f, z = 0.0f, r = 8.0f;
		sscanf(getenv("AIRACE_WALLS_AT"), "%f,%f,%f,%f", &x, &y, &z, &r);
		static Model3DWall wl[96];
		const int nw = topo_walls_near(g_world.topo, TOPO_COORD_LINE, x, y, r, z + 0.35f, z + 3.0f,
			wl, 0, 96);
		for (int i = 0; i < nw; i++)
			printf("  wallat (%.2f,%.2f)-(%.2f,%.2f) z %.2f..%.2f\n", wl[i].x0, wl[i].y0, wl[i].x1,
				wl[i].y1, wl[i].zmin, wl[i].zmax);
	}

	// The line's own clearance: how near its world point comes to a wall in
	// the body's band, every 2 m, and the places it is under CLEAR_WARN_M.
	if (f.line && !g_noWalls) {
		const float warn = CLEAR_WARN_M;
		int nWarn = 0, nSpots = 0;
		float spotD = -1e9f, spotMin = 1e9f, spotX = 0.0f, spotY = 0.0f;
		char spots[512] = "";
		for (float d = 0.0f; d <= len; d += 2.0f) {
			AiLineSample s;
			float p[2];
			if (!ailine_at(f.line, d, &s) || !ailine_pose(f.line, d, 0.0f, p, NULL)) continue;
			AiLineWall wl[96];
			float gz = s.z;
			if (!lineGroundThunk(NULL, p[0], p[1], s.z + 1.5f, &gz) || fabsf(gz - s.z) > 3.0f)
				gz = s.z;
			const int nw = lineWallsThunk(NULL, p[0], p[1], warn + 1.0f, gz + 0.35f,
				gz + 3.0f, wl, 96);
			float best = 1e9f;
			int bi = -1;
			for (int i = 0; i < nw; i++) {
				const float dx = wl[i].x1 - wl[i].x0, dy = wl[i].y1 - wl[i].y0;
				const float l2 = dx * dx + dy * dy;
				float u = l2 > 1e-9f ? ((p[0] - wl[i].x0) * dx + (p[1] - wl[i].y0) * dy) / l2 : 0.0f;
				u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
				const float ex = wl[i].x0 + u * dx - p[0], ey = wl[i].y0 + u * dy - p[1];
				const float e = sqrtf(ex * ex + ey * ey);
				if (e < best) { best = e; bi = i; }
			}
			if (getenv("AIRACE_CLEAR_FROM") && d >= atof(getenv("AIRACE_CLEAR_FROM")) &&
					d <= atof(getenv("AIRACE_CLEAR_TO")))
				printf("  clear %.0f at (%.1f,%.1f) z %.2f ground %.2f walls %d best %.2f\n", d,
					p[0], p[1], s.z, gz, nw, best),
				bi >= 0 && printf("    wall (%.1f,%.1f)-(%.1f,%.1f)\n", wl[bi].x0, wl[bi].y0,
					wl[bi].x1, wl[bi].y1);
			if (best >= warn) continue;
			nWarn++;
			if (d - spotD > 20.0f) {
				if (spotD > -1e8f && nSpots < 12) {
					char one[64];
					snprintf(one, sizeof(one), " %.0f(%.1f@%.0f,%.0f)", spotD, spotMin, spotX, spotY);
					strncat(spots, one, sizeof(spots) - strlen(spots) - 1);
					nSpots++;
				}
				spotMin = 1e9f;
			}
			spotD = d;
			if (best < spotMin) { spotMin = best; spotX = p[0]; spotY = p[1]; }
		}
		if (spotD > -1e8f && nSpots < 12) {
			char one[64];
			snprintf(one, sizeof(one), " %.0f(%.1f@%.0f,%.0f)", spotD, spotMin, spotX, spotY);
			strncat(spots, one, sizeof(spots) - strlen(spots) - 1);
		}
		printf("CLEAR %d%s line under %.1f m from a wall at %d sample(s):%s\n", track,
			reverse ? "B" : "F", warn, nWarn, spots);
	}

	static Watch w[OPPONENT_MAX];
	memset(w, 0, sizeof(w));
	for (int k = 0; k < f.count; k++) {
		w[k].lastDist = f.car[k].coord.dist;
		// A sprint's grid need not be at its route's start (4126F's is 800 m
		// up it), and its finish is the route's end.
		if (!loops) w[k].progress = f.car[k].coord.dist;
		w[k].prevSinceReset = 0.0f;
		w[k].launchSec = -1.0f;
	}

	// The countdown, then the race.
	const float countdown = 3.0f;
	float sec = -countdown;
	for (; sec < maxSec; sec += dt) {
		const bool racing = sec >= 0.0f;
		memset(g_hit, 0, sizeof(g_hit));
		for (int k = 0; k < f.count; k++) f.car[k].finished = w[k].finished;
		opponent_update(&f, route, &fallback, NULL, racing, dt);
		// AIRACE_WATCH=<driver> with AIRACE_WATCH_FROM / _TO (route metres): that
		// car frame by frame -- where it is, what it steers at and what it does.
		if (watchName) {
			for (int k = 0; k < f.count; k++) {
				const Opponent* o = &f.car[k];
				if (!o->simReady || strcmp(o->name, watchName) || o->lineDist < watchFrom ||
						o->lineDist > watchTo) continue;
				printf("W %6.2f %s d %7.1f at (%7.1f,%7.1f,%5.1f) hdg %6.1f v %5.1f want %5.1f "
					"to (%7.1f,%7.1f) lat %+5.1f steer %+6.1f thr %.2f brk %.2f "
					"L %+5.2f cur %+5.2f goal %+5.2f @%.2f%s\n", sec, o->name,
					o->lineDist, o->sim.pos[0], o->sim.pos[1], o->sim.pos[2],
					atan2f(o->sim.rot[0][1], o->sim.rot[0][0]) * 57.2958f, o->sim.speed,
					o->desired, o->drv.steerTo[0], o->drv.steerTo[1], o->drv.lateral_m,
					o->lastSteerDeg, o->lastThr, o->lastBrk, o->lineLat, o->curL, o->goalL,
					o->latRate, o->unstuck ? " UNSTUCK" : "");
				if (getenv("AIRACE_WATCH_PID"))
					printf("   PID eH %+6.1f eB %+6.1f dH %+6.2f dB %+6.2f vel %+6.1f yaw %+6.1f\n",
						o->drv.heading.last * 57.2958f, o->drv.blended.last * 57.2958f,
						o->drv.heading.deriv.avg, o->drv.blended.deriv.avg,
						atan2f(o->sim.vel[1], o->sim.vel[0]) * 57.2958f, o->sim.yaw * 57.2958f);
				// AIRACE_WATCH_WHEELS=1 adds the yaw rate, the body slip angle and
				// each wheel's state (L locked, S sliding, - gripping, . airborne).
				if (watchWheels) {
					const CarSim* s = &o->sim;
					float hdg = atan2f(s->rot[0][1], s->rot[0][0]);
					float slipAng = s->speed > 1.0f ?
						atan2f(s->vel[1], s->vel[0]) - hdg : 0.0f;
					while (slipAng > 3.14159f) slipAng -= 6.28319f;
					while (slipAng < -3.14159f) slipAng += 6.28319f;
					// the road's cross slope under the car: its normal on the car's left axis
					float nl = s->wheel[0].n[0] * s->rot[1][0] + s->wheel[0].n[1] * s->rot[1][1] +
						s->wheel[0].n[2] * s->rot[1][2];
					printf("WW yawrate %+6.1f slip %+6.1f roll %+5.1f bank %+5.1f wheels",
						s->w[2] * 57.2958f, slipAng * 57.2958f, s->roll * 57.2958f,
						asinf(nl) * 57.2958f);
					for (int wi = 0; wi < CARPHYS_WHEELS; wi++) {
						const CarSimWheel* w = &s->wheel[wi];
						printf(" %c%.2f/%.0f", !w->onGround ? '.' : w->locked ? 'L' :
							w->sliding ? 'S' : '-', w->gripUse, w->Fz);
					}
					printf("\nWS");
					for (int wi = 0; wi < CARPHYS_WHEELS; wi++)
						printf(" x %+.3f xd %+.3f F %+6.0f g %d |", s->susp[wi].x, s->susp[wi].xdot,
							s->susp[wi].F, s->susp[wi].onGround);
					printf("\n");
				}
			}
		}
		bool touching[OPPONENT_MAX] = { false };
		int touchWho[OPPONENT_MAX];
		for (int k = 0; k < f.count; k++) {
			Opponent* o = &f.car[k];
			if (!o->simReady || opponent_blinking(o)) continue;
			for (int m = k + 1; m < f.count; m++) {
				Opponent* p2 = &f.car[m];
				if (p2->simReady && !opponent_blinking(p2) &&
						carsim_car_contact(&o->sim, &p2->sim, dt))
				{
					opponent_car_contact(&f, k, m);
					touching[k] = touching[m] = true;
					touchWho[k] = m;
					touchWho[m] = k;
				}
			}
		}
		if (!racing) continue;

		bool all = true;
		for (int k = 0; k < f.count; k++) {
			Opponent* o = &f.car[k];
			Watch* m = &w[k];
			if (!o->simReady || !o->driving) continue;
			if (m->finished) continue;
			all = false;

			float d = o->coord.dist - m->lastDist;
			if (loops) {
				if (d < -0.5f * len) d += len;
				else if (d > 0.5f * len) d -= len;
			}
			m->progress += d;
			m->lastDist = o->coord.dist;
			if (m->progress >= goal - 1.0f) {
				m->finished = true;
				m->finishSec = sec;
				continue;
			}

			m->sumSpeed += o->sim.speed;
			m->samples++;
			if (o->sim.mellow) m->mellowSec += dt;
			if ((o->sim.mellow != 0) != m->prevMellow && m->samples > 1) m->moverSwaps++;
			m->prevMellow = o->sim.mellow != 0;
			if (o->sim.speed > m->maxSpeed) m->maxSpeed = o->sim.speed;

			const bool wall = g_hit[(int)(intptr_t)o->world.ctx];
			// AIRACE_AI_DEBUG=name: that car's place on the line and its choice, every 0.1 s.
			if (aiDbg && !strcmp(aiDbg, o->name) && ((int)(sec / dt) % 3) == 0)
				printf("AI %6.2f d %6.1f lat %+5.2f cur %+5.2f goal %+6.2f rate %.2f avoid %d spd %4.1f wall %d\n",
					sec, o->lineDist, o->lineLat, o->curL, o->goalL, o->latRate, o->avoiding,
					o->sim.speed, (int)wall);
			// AIRACE_STEER_DEBUG=name: that car's steering controller, every 0.25 s.
			if (steerDbg && !strcmp(steerDbg, o->name) && ((int)(sec / dt) % 8) == 0)
				printf("STEER %6.2f d %6.0f spd %5.1f lat %+5.1f coef %.3f %.3f %.3f "
					"term %+.3f %+.3f %+.3f err %+6.2f u16 %d mellow %d\n", sec,
					o->coord.dist, o->sim.speed, o->drv.lateral_m, o->drv.steer.coef[0],
					o->drv.steer.coef[1], o->drv.steer.coef[2], o->drv.steer.term[0],
					o->drv.steer.term[1], o->drv.steer.term[2], o->drv.steer.lastErr,
					(int)(short)o->drv.steerU16, o->sim.mellow);
			// AIRACE_THR_DEBUG=name: that car's pedals and throttle controller, every 0.25 s.
			if (thrDbg && !strcmp(thrDbg, o->name) && ((int)(sec / dt) % 8) == 0)
				printf("THR %6.2f d %6.0f spd %5.1f want %5.1f thr %.2f brk %.2f coef %+.3f %+.3f "
					"%+.3f term %+6.2f %+6.2f %+6.2f\n", sec, o->lineDist, o->sim.speed,
					o->drv.desired, o->drv.prevThrottle, o->drv.prevBrake,
					o->drv.throttleAdaptive.coef[0], o->drv.throttleAdaptive.coef[1],
					o->drv.throttleAdaptive.coef[2], o->drv.throttleAdaptive.term[0],
					o->drv.throttleAdaptive.term[1], o->drv.throttleAdaptive.term[2]);
			if (trace && ((int)(sec / dt) % 3) == 0)
				fprintf(trace, "C,%s,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f,%.1f,%d,%d\n", o->name, sec,
					o->sim.pos[0], o->sim.pos[1], o->sim.pos[2], o->coord.dist,
					o->drv.lateral_m, o->sim.speed, (int)wall, (int)o->unstuck);
			if (wall) m->wallSec += dt;
			if (touching[k]) {
				m->carSec += dt;
				if (!m->prevCar) {
					m->carHits++;
					const Opponent* q = &f.car[touchWho[k]];
					printf("TOUCH %6.1f s %-10s by %-10s at %6.0f m lat %+5.1f / %+5.1f "
						"goal %+5.1f / %+5.1f speed %4.1f / %4.1f\n", sec, o->name, q->name,
						o->lineDist, o->lineLat, q->lineLat, o->goalL, q->goalL,
						o->sim.speed, q->sim.speed);
				}
			}
			m->prevCar = touching[k];
			spell(o, m, I_WALL, wall, WALL_RUN_SEC, dt, sec);

			const float off = fabsf(o->drv.lateral_m);
			if (!o->unstuck && off > m->worstOff) m->worstOff = off;
			if (off > OFFLINE_M) m->offSec += dt;
			spell(o, m, I_OFFLINE, !o->unstuck && off > OFFLINE_M, OFFLINE_SEC, dt, sec);

			if (m->launchSec < 0.0f && fabsf(o->sim.speed) >= STALL_SPEED) m->launchSec = sec;
			const bool slow = sec > START_GRACE_SEC && fabsf(o->sim.speed) < STALL_SPEED;
			if (slow) m->stallSec += dt;
			spell(o, m, I_STALL, slow, STALL_SEC, dt, sec);

			// Against its own line where it has one: the race route's own
			// direction on a branch path down the other carriageway is not.
			float ld[2] = { o->coord.dir[0], o->coord.dir[1] };
			ailine_pose(f.line, o->lineDist, 0.0f, NULL, ld);
			const float along = o->sim.rot[0][0] * ld[0] + o->sim.rot[0][1] * ld[1];
			spell(o, m, I_WRONG, !o->unstuck && along < 0.0f, WRONG_SEC, dt, sec);
			spell(o, m, I_AIR, !o->sim.onGround, AIR_SEC, dt, sec);

			if (o->sinceReset < m->prevSinceReset) incident(o, m, I_RESET, sec, NULL);
			m->prevSinceReset = o->sinceReset;
			// A blink begun without a reset is opponent.c's hop past a face.
			if (o->blinkSec > m->prevBlink + 0.5f && o->sinceReset > 0.0f)
				incident(o, m, I_HOP, sec, NULL);
			m->prevBlink = o->blinkSec;
			if (o->unstuck && !m->prevUnstuck) incident(o, m, I_UNSTUCK, sec, NULL);
			m->prevUnstuck = o->unstuck;
		}
		if (all) break;
	}

	int bad = 0;
	for (int k = 0; k < f.count; k++) {
		const Opponent* o = &f.car[k];
		const Watch* m = &w[k];
		if (!o->simReady) continue;
		const float avg = m->samples ? m->sumSpeed / (float)m->samples : 0.0f;
		printf("RESULT %d%s %-10s %-8s %s %7.1f s avg %5.1f top %5.1f prog %6.0f/%-6.0f "
			"wall %5.1f s off %5.1f s (worst %4.1f) stall %5.1f s | wall %d off %d stall %d "
			"wrong %d air %d reset %d unstuck %d hop %d cars %.1f s %d mellow %.0f s %d launch %.1f s\n",
			track, reverse ? "B" : "F", o->name, o->carDir,
			m->finished ? "FIN" : "DNF", m->finished ? m->finishSec : sec, avg,
			m->maxSpeed, m->progress, goal, m->wallSec, m->offSec, m->worstOff,
			m->stallSec, m->count[I_WALL], m->count[I_OFFLINE], m->count[I_STALL],
			m->count[I_WRONG], m->count[I_AIR], m->count[I_RESET], m->count[I_UNSTUCK],
			m->count[I_HOP], m->carSec, m->carHits, m->mellowSec, m->moverSwaps, m->launchSec);
		if (!m->finished || m->count[I_RESET] || m->count[I_STALL] || m->count[I_WRONG])
			bad++;
	}
	if (trace) fclose(trace);
	opponent_free(&f);
	race_markers_close(mk);
	race_route_close(route);
	topo_close(g_world.topo);
	return bad ? 3 : 0;
}
