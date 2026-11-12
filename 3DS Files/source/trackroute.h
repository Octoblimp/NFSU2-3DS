#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "race.h"

// Retail's AI roads: TrackRouteManager's TrackRoutes (TASKS.md #272).
//
// A racer in Speed.elf does not drive the race's TrackPath spline. The spline
// is what the race measures progress along, and in places it runs where no car
// can -- on Bayview route 4164 it cuts down the walled median between the two
// carriageways from about 360 m to 405 m. What an AICarController drives is a
// TrackRoute: a chain of TrackRoutePoints down the middle of a real road, each
// carrying the road's two half-widths and an offset per driving line, loaded
// per race from
//
//     TRACKS/ROUTESL4RA/Routes<track><F|B>.bin
//
// (the format string at 0x8032E3B4, built at 0x80196230: "F", or "B" when
// TheRaceParameters+0x04 says the race runs reversed).
//
// The file is the flat {u32 type, u32 size} chunk list LoaderTrackRoutes
// (0x801FF230) walks; chunk 0x34121 holds the routes back to back, each 0x8C
// of header (s16 point count at +0x34) then that many 0x38-byte points. What
// this reads of a point, with where each was read from:
//
//   +0x00 float x, +0x04 float y
//   +0x0C u32 bits, GetBits (0x80302508): bit 11 posted for racers forward
//         (0x8030242C), bit 18 posted for racers backward (0x80302458) --
//         IsPostedDirectional (0x801F8274) with ePostednessType 0
//   +0x12 s16 z, x 8192 / 65536 (ResetToRoutePoint 0x80221314,
//         FindClosestTrackRoute 0x80006968)
//   +0x14 u16 half-width to the left of the route's direction, /256 m
//   +0x16 u16 half-width to the right, /256 m
//   +0x22 s8[10] driving-line offsets, /128 of the wider half-width, positive
//         to the right of that line's own direction of travel
//         (SetDrivingLineOffset 0x80312AF0, GetForwardsDrivingLineOffset
//         0x801F846C). A racer drives line 0 down a route and line 1 up it
//         (IsForwardsRacingLine, MakeDrivingLineNumberBackwards 0x8001894C);
//         GetRacingLineNumberForCar (0x800189FC) answers 0 for every racer.
//
// PORT: retail's AI keeps a current route and point, walks FuturePoints along
// it, and picks the next route at each signpost by its relevance to the track
// path (DetermineRelevanceOfRoutesToTrackPath 0x801FE998). None of that
// machinery is here. Instead the posted routes are matched to the race route
// once, at race start: every AILINE_STEP metres along the race route the
// posted route segment that road belongs to is found the way
// FindClosestTrackRoute finds one (inside the road's half-widths if any is,
// else the nearest), preferring one that runs the race's way, and its racing
// line, centre line, edges and height are stored across the race route. The
// AI then steers at that line instead of at the spline itself, and a reset
// puts the car on the route point at the route's own height, which is what
// ResetToRoutePoint does.

typedef struct TrackRouteSet TrackRouteSet;
typedef struct AiLine AiLine;

// Reads a Routes file. NULL when it is absent or holds no routes; `log` may be
// NULL.
TrackRouteSet* trackroute_open(const char* path, void (*log)(const char* msg));
void trackroute_close(TrackRouteSet* s);

// ------------------------------------------ the player's reset (TASKS.md #279)
//
// GetNearestRoutePoint (Speed.elf 0x80016F00): over every TrackRoute the
// manager holds, TrackRoute::FindClosestPoint (0x801F86A8) picks the route's
// nearest point under the best found so far, and GetNearestRoutePoint keeps it
// if it passes the tests the flags ask for. Player::HandleReset (0x80219D38)
// asks with 0x1420 when the route point the car was on is behind a wall.
//
// The manager holds whichever file the mode loaded -- the race's
// Routes<track><F|B>.bin, or RoutesFreeRoam.bin in free roam -- so the search
// runs over either: a TrackRouteSet (trackroute_source) or the GPS's copy of
// the free-roam file (gps_route_source).

// One TrackRoutePoint as the search reads it.
typedef struct {
	float pos[3];
	float fwd[2];     // GetForwardDirection (0x8030230C)
	float back[2];    // GetBackwardDirection (0x8030237C): the previous
	                  // point's reversed, or this one's at a route's start
	float raceDist;   // +0x08, metres along the race (0 in RoutesFreeRoam)
	bool  posted;     // GetPostedForPlayersForward (0x80312BC0), bit 27
	u8    speed;      // +0x2C, driving line 0's speed, m/s: Player::
	                  // UpdateGameState's speed-ratio sample (TASKS.md #255)
} TrackRoutePt;

typedef struct {
	const void* ctx;
	int nRoutes;
	// Route r's points (`first` and `count`, as indices for `point`), its
	// bounding box (+0x60 / +0x68) and its bits (+0x70).
	void (*route)(const void* ctx, int r, int* first, int* count, float bbMin[2],
		float bbMax[2], u32* bits);
	void (*point)(const void* ctx, int p, TrackRoutePt* out);
} TrackRouteSource;

// GetNearestRoutePoint's flags, by retail's own values. The rest -- 0x40, the
// car inside the point's road box, and 0x80 / 0x100, routes with or without
// traffic lanes -- have no caller in the port.
#define TRN_RACE_DIST 0x0020 // while racing, also by the race's distance
#define TRN_PLAYERS   0x0400 // points posted for players only
#define TRN_NEAR      0x0800 // within 100 m (999999 without)
#define TRN_WALLS     0x1000 // within 90 m: no wall between, none within 1.5 m
#define TRN_LEVEL     0x2000 // within 4 m of the car's height

typedef struct {
	float pos[3];     // Car +0x58
	int   flags;      // TRN_*
	bool  racing;     // TheTrackPathManager +0x2C (0x803F4494)
	float raceDist;   // Car +0x8EC, the car's TrackPathCoordinate distance
	float loopLen;    // the race route's length when it loops, else 0
	// TRN_WALLS's two topology questions: SegmentIntersectsPossiblyLong
	// (0x801F1DD8), whether `a` to `b` goes through a wall, and
	// PointIsCloserThanThisToWall (0x801F23AC). Either may be NULL.
	bool (*wallBetween)(void* ctx, const float a[3], const float b[3]);
	bool (*wallNear)(void* ctx, const float p[3], float d);
	void* ctx;
} TrackRouteQuery;

typedef struct {
	int route, point;  // the source's indices
	u32 routeBits;     // the route's +0x70
	float dist;        // GetDistanceAway
	TrackRoutePt pt;
} TrackRoutePick;

// False for no point that passes, or a NULL or empty source.
bool trackroute_nearest(const TrackRouteSource* src, const TrackRouteQuery* q,
	TrackRoutePick* out);
// The set as a source; no routes when `s` is NULL.
TrackRouteSource trackroute_source(const TrackRouteSet* s);

// The solid walls near a point, for ailine_build's last pass (TASKS.md #280):
// the same TopologyTree walls a car collides with, as XY segments, whose height
// band overlaps [zLo, zHi]. Returns how many it wrote to `out`.
typedef struct {
	float x0, y0, x1, y1;
} AiLineWall;
typedef struct {
	int (*near)(void* ctx, float x, float y, float radius, float zLo, float zHi,
		AiLineWall* out, int max);
	void* ctx;
	// The road's height under (x, y), at or below zMax; false for none. May be
	// NULL, when the route's own z stands in for the road's.
	bool (*ground)(void* ctx, float x, float y, float zMax, float* outZ);
} AiLineWalls;

// The posted routes matched along `route`. `startZ` is the height of the grid,
// which picks the right deck where roads cross at the start. `gridXY` holds
// `nGrid` (x, y) pairs, where the player and the field start: where they are
// on a road the line's own cannot be reached from, the line follows theirs
// until it rejoins (AILINE_GRID_*); NULL for none. `walls`, when not NULL, is
// the race's collision: wherever the matched line would put a car into a wall
// it is moved clear, and the move is ramped in ahead of the wall rather than
// made at it. NULL when nothing on the race route matches a posted road.
AiLine* ailine_build(const TrackRouteSet* s, const RaceRoute* route, float startZ,
	const float* gridXY, int nGrid, const AiLineWalls* walls, void (*log)(const char* msg));
void ailine_free(AiLine* t);

// What the line holds at one distance along the race route. Every lateral is
// metres to the right of the race route there, as race_route_sample takes it.
typedef struct {
	float line;      // the racing line a racer drives (line 0, or 1 up a route)
	float point;     // the TrackRoutePoint chain itself, where a reset puts a car
	float left;      // the road's left edge, seen along the race
	float right;     // and its right edge
	float z;         // the route's height
	float dir[2];    // the route's direction of travel, the race's way
} AiLineSample;

// `dist` wraps on a looped route and clamps on a sprint. False when `t` is NULL.
bool ailine_at(const AiLine* t, float dist, AiLineSample* out);

// The line as a car drives it (TASKS.md #280). A car finds itself on the line,
// not on the race route: the race route's TrackPathCoordinate may stand on a
// branch path running down the other carriageway, and a lateral measured from
// that while the steer point is on the line had a car pull one way and steer
// the other, along a median wall, for half a kilometre.
//
// The world point `lateral` metres right of the line at `dist`, and the line's
// own heading there. False when `t` is NULL.
bool ailine_pose(const AiLine* t, float dist, float lateral, float outPos[2],
	float outDir[2]);
// Where `pos` is along the line: the nearest point within `back` metres behind
// and `ahead` metres beyond `dist` (a car's last answer, or the race route's
// for a car just placed), returned as a distance, with how far `pos` is to the
// right of the line there in `outLateral` and the line's heading in `outDir`
// (either may be NULL). Returns `dist` unchanged when `t` is NULL.
float ailine_track(const AiLine* t, float dist, float back, float ahead,
	const float pos[2], float* outLateral, float outDir[2]);

// The line's own length (TASKS.md #280). A distance is the race route's, and
// where the line leaves the race route's road a metre of it is not a metre of
// the line: 4002F's grid road turns across the route, 40 m of road in 8 m of
// the route's. Anything retail measures along its own route -- a car's
// look-ahead, the speed profile's corners and braking -- is measured in these.
// ailine_arc: the line's world metres from its start to `dist`, a loop's laps
// counting on and a sprint running straight on past either end.
// ailine_along: the distance `metres` of the line on from `dist` (back for a
// negative), unwrapped the same way. `dist + metres` when `t` is NULL.
float ailine_arc(const AiLine* t, float dist);
float ailine_along(const AiLine* t, float dist, float metres);

// A wall the race route goes through with no way round it (TASKS.md #280):
// how far along the route, and the road's height just before and just after.
// Walls a few metres apart are one crossing, from the first to the last
// (`dist` to `end`), the heights before the first and after the last.
typedef struct {
	float dist, end, zBefore, zAfter;
} AiLineCrossing;
// The first one no more than `within` metres beyond `from` (round the loop on
// a circuit), and whose last wall is still ahead, that is a face up -- the
// road beyond it higher than a car can climb, which a drop off a ledge is
// not. False for none, or when `t` is NULL.
bool ailine_face_ahead(const AiLine* t, float from, float within, AiLineCrossing* out);

// The traffic lanes along the line (TASKS.md #268 D, a drag). Retail's drag
// racers keep to the TrackRoutePoints' traffic lanes -- lines 2..5 the way a
// route runs, 6..9 against it -- and move between them a lane at a time
// (GetNearestTrafficLane 0x80018650, DragController, AIActionDriveTrafficLane).
// Each sample of the line takes every lane off a road posted for racers the
// race's way, as metres to the right of the line: the lateral ailine_pose
// takes. Lanes are numbered 2..5 whichever way the road is driven, as retail's
// GetNearestTrafficLane returns them. The table keeps `line`, which must
// outlive it. NULL where no sample of the line is on a road with lanes.
//
// PORT: retail reads the lanes of the route a car is on, and a car moves from
// route to route as each ends. Here each lane keeps to the road it is on,
// along the line, until another that has it runs further -- so where a race
// splits, each lane follows its own road: 4201F's lanes 2-3 take one road
// past the post at 1265 m and 4-5 another, up to 24 m apart across and 9 m in
// height, until both join r7 at 1839 m.
#define TRACK_LANE_FIRST 2
#define TRACK_LANE_LAST  5
typedef struct TrackLanes TrackLanes;
TrackLanes* trackroute_lanes(const TrackRouteSet* s, const AiLine* line,
	void (*log)(const char* msg));
void trackroute_lanes_free(TrackLanes* t);
// A lane where it is: its lateral, the edges of the road it is on (metres
// right of the line) and that road's height.
typedef struct {
	float lat, lo, hi, z;
} TrackLaneRoad;
// Lane `lane` at `dist`; false where no road there has such a lane.
bool trackroute_lane_road(const TrackLanes* t, int lane, float dist, TrackLaneRoad* out);
// Lane `lane`'s lateral at `dist`; false where no road there has such a lane.
bool trackroute_lane_at(const TrackLanes* t, int lane, float dist, float* outLat);
// How far on from `dist` lane `lane` runs without a break, up to `cap` metres
// (`cap` where it runs to a sprint's end); 0 where it is not there at `dist`.
float trackroute_lane_run(const TrackLanes* t, int lane, float dist, float cap);

// The race's forks (TASKS.md #409): where a racer may leave the line for a
// branch road and rejoin it. A Signpost (chunk 0x34122) stands where one route
// meets another; AIGoalWinRoadRace asks MustTakeSignpost -- IsAlways(0), no
// sign in a shipped race -- and then WantToTakeSignpost, AIGoal::
// DecideSignpostAsRacer (0x8000A914). For a sign of racer type 4 that is not
// a drag's it takes the new route when bRandom(1) < 0.5 (DAT_8031601C), or
// 0.333 (DAT_80316020) with the sign's bit 27, once the track allows it:
// TrackInfo +0xE4 -1, after Race::GracePeriodIsActive; otherwise after that
// many seconds of the race. The 60 race files carry 892 such signs, onto
// racer-posted branches 100-200 m long 4-60 m off the course.
//
// PORT: retail's racer leaves its route at the sign and follows the new one
// to its end. The port's racers drive the line (ailine_build), so each sign
// the race's way round is a fork: the branch's racing line, as metres right
// of the line across the samples it runs beside, blended in and out over a
// few samples either side. A branch the line already follows is no fork.
// Built on `line`, which must outlive the table; NULL for none.
typedef struct TrackForks TrackForks;
TrackForks* trackroute_forks(const TrackRouteSet* s, const AiLine* line,
	void (*log)(const char* msg));
void trackroute_forks_free(TrackForks* t);
int trackroute_fork_count(const TrackForks* t);
typedef struct {
	float start;   // where its blend in starts, along the line
	float sign;    // where the sign stands
	float span;    // metres from `start` to the end of its blend back
	float p;       // the chance a racer takes it
	int branch;    // the branch's route number
} TrackFork;
bool trackroute_fork_info(const TrackForks* t, int i, TrackFork* out);
// Fork `i`'s road at `dist`: the branch's racing line, its edges and height,
// metres right of the line; false outside its span.
bool trackroute_fork_road(const TrackForks* t, int i, float dist, TrackLaneRoad* out);
// The curvature (1/m, unsigned) of the path fork `i`'s racer drives at
// `dist`, blends and all; false outside its span.
bool trackroute_fork_curv(const TrackForks* t, int i, float dist, float* curv);
// Metres on from `dist` to fork `i`'s sign, negative once past; round a loop
// the short way.
float trackroute_fork_ahead(const TrackForks* t, int i, float dist);
