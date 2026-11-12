#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "trackroute.h"

// Retail's free-roam GPS (TASKS.md #217, #275): the route search, the path
// follow and the arrow above the car.
//
// Four retail objects, each ported as one static instance here because retail
// keeps exactly one of each:
//
//   ThePathfindingManager  a time-sliced A* over TRACKS/ROUTESL4RA/
//                          RoutesFreeRoam.bin's TrackRoutes, from the car to a
//                          goal. Its answer is one PathfindingNode: the start
//                          point, the start direction and the list of signpost
//                          decisions (take / don't take) along the way.
//   TheGPSManager          what the career and the world map talk to: Start on
//                          a trigger zone's GPS position, Stop, and the status
//                          the map and the minimap print.
//   RealController's GPS   UpdateGPSFuturePoints (0x800326C4): keeps the
//                          player's GPS FuturePoints -- up to 40 points walked
//                          down the route by the path's decisions -- in front
//                          of the car, re-finding its place on the path when
//                          the car leaves the road and asking for a new search
//                          when it cannot.
//   Player::RenderGPSArrow (0x8021800C): the arrow's target is the point
//                          CalculateGPSArrowLookaheadDistance metres down that
//                          path from the car, and RenderArrow (0x802181AC)
//                          hangs MARKER_GPS_AID above the middle of the screen
//                          pointing at it.
//
// Every function below names the retail function it transcribes; the PORT:
// notes in gps.c say where the port takes a narrower path.

// GPSManager::GetGPSStatus (0x8012ADA0). The world map's GPS text and the
// minimap's icon are chosen by these.
enum {
	GPS_STATUS_SEARCHING = 0, // the search is running
	GPS_STATUS_LOST      = 1, // no posted road under the car to start from
	GPS_STATUS_NO_PATH   = 2, // started on a road, but no path reaches the goal
	GPS_STATUS_ACTIVE    = 3, // a path is being followed
	GPS_STATUS_IDLE      = 4, // not started
};

// What the four objects read off the player's car each frame.
typedef struct {
	float pos[3];     // Car +0x58, world metres
	float fwd[2];     // the car's forward axis, x and y (SubmitRequest's
	                  // heading; only its sign against a road matters)
	float speed;      // Car +0x440, m/s
	float halfWidth;  // half the car-space bounding box's y extent
} GpsCar;

// Loads the free-roam routes. False when the file is missing or holds no
// routes; every other call is then a no-op. `log` may be NULL.
bool gps_load(const char* routesPath, void (*log)(const char* msg));
void gps_unload(void);
bool gps_loaded(void);

// A race's own routes, TRACKS/ROUTESL4RA/Routes<track><F|B>.bin, for its
// traffic (TASKS.md #316): retail's TrackRouteManager holds that file in place
// of RoutesFreeRoam.bin while a race is on, and the TrafficTeleporter puts the
// race's traffic down on its routes. The port keeps both and the road-network
// calls further down read whichever gps_net_select() last chose; everything
// else here -- the search, the follow, the arrow -- is free roam's and must
// only run with GPS_NET_FREEROAM selected. gps_race_unload() selects free
// roam again.
enum { GPS_NET_FREEROAM = 0, GPS_NET_RACE = 1 };
bool gps_race_load(const char* routesPath, void (*log)(const char* msg));
void gps_race_unload(void);
// Returns the network selected before.
int  gps_net_select(int net);

// InitTrackRoutes' free-roam barrier pass (TASKS.md #358) on the free-roam
// network: the gates of `pathsFile` (PathsFreeRoam.bin's 0x3414D) holding one
// of `ids` -- bStringHash of the names EnableBarriers is given -- are closed,
// and the signposts and posted bits redone from the file's as retail does at
// load. A repeat of the last set does nothing (-1). Logs one line.
int  gps_set_barriers(const char* pathsFile, const u32* ids, int nIds, void (*log)(const char* msg));

// The free-roam routes as trackroute_nearest's source (TASKS.md #279): in free
// roam retail's TrackRouteManager holds RoutesFreeRoam.bin, and that is what
// Player::HandleReset searches. No routes when nothing is loaded.
TrackRouteSource gps_route_source(void);

// The player's driving FuturePoints (RealController +0x68, UpdateFuturePoints
// 0x80032404), built once on demand for Player::IsJumpCamAvailable
// (0x802127E4) and the crash camera it opens (TASKS.md #335): from route
// point `start` of the selected network, the way `fwd` says, FillUpArray's
// walk with the GPS instance's code -- a point per route point, gaps over
// 16 m cut every 8 m, at most 40 -- until `need` metres. The GPS's own array
// is left as it was. Returns the count; `*closest` is GetClosestPoint's pick
// for `carPos`. Each point is kept with what the camera reads, so it stays
// valid whichever network is selected afterwards.
typedef struct {
	float D;          // +0x50: metres down the path from the first point
	float pos[3];     // GetPosition(D, L = 0)
	float right[2];   // a metre to the right of the path, in x and y
	float zPerL;      // and in height (the road's cross slope)
	float minL, maxL; // GetUnadjustedMinL / MaxL: the road's edges, L metres
} GpsPfpPoint;
int  gps_pfp_build(int start, bool fwd, const float carPos[3], float need, int* closest);
const GpsPfpPoint* gps_pfp_point(int i);   // NULL past the last
// GetPosition(D of point i, L): L metres right of the path at the point.
void gps_pfp_position(int i, float L, float out[3]);

// GPSManager::Start / Stop / GetGPSStatus / IsTriggerZoneTheGPSLocation.
// `target` is UnlockedTriggerZone::GetGPSPosition -- the zone's three ints at
// +0x34 as floats -- and `icon` the zone's entry point (+0x04), which is where
// the minimap draws the GPS icon (GetMinimapArtCenter).
void gps_start(const GpsCar* car, const float target[3], const float icon[2]);
void gps_stop(void);
int  gps_status(void);
bool gps_is_target(const float target[3]);
// The icon position while started; false when the GPS is idle.
bool gps_icon(float out[2]);
// RealController +0x3EC, the follow's eGPSState: 0 off, 1 following, 3 no
// path. MinimapWrapper::UpdateGPS hides the searching group while it reads 1
// or 2.
int  gps_follow_state(void);

// One frame of the free-roam GPS: ThePathfindingManager.Service, then
// RealController::UpdateGPSFuturePoints. The search gets retail's slice: 1 ms
// a frame with 0.4 ms of headroom.
void gps_update(const GpsCar* car);

// Player::RenderGPSArrow's target choice: true with the world point the arrow
// should point at. `cache` mirrors retail's +0x114/+0x118 -- when the path is
// briefly unavailable the last target is used. Call once per rendered frame;
// gps_arrow_reset() is retail's clearing of +0x114 whenever the arrow is not
// drawn at all.
bool gps_arrow_target(const GpsCar* car, float out[3]);
void gps_arrow_reset(void);

// Player::RenderArrow's model matrix, row-vector form: world = x*rows[0] +
// y*rows[1] + z*rows[2] + rows[3]. `eye`, `fwd` and `up` are the camera's
// world position and its forward and screen-up axes; `fovU16` the camera's
// field of view in retail's 65536-per-turn units (cam+0xC4).
void gps_arrow_matrix(const float eye[3], const float fwd[3], const float up[3],
	u16 fovU16, const float target[3], float rows[4][3]);

// Diagnostics for the log: the search's open and closed counts, the follow
// state and the number of future points.
void gps_debug(int* state, int* pmState, int* fpCount, int* pathDecisions);

// ---------------------------------------------------------------------------
// The road network, for traffic (TASKS.md #316)
//
// Traffic drives the same RoutesFreeRoam.bin graph the GPS searches, so the
// loaded routes are read here with TrackRoute, TrackRoutePoint and Signpost's
// own accessors. A point is an index into the file's points, all routes back
// to back; a route is an index in file order; a signpost an index in its
// chunk. Every call returns 0 / -1 / false while nothing is loaded.
//
// The driving lines: 0-1 are the racing lines, 2-5 the forward traffic lanes
// (IsForwardsTrafficLane, 0x800188B4) and 6-9 the backward ones (IsTrafficLane,
// 0x800188D8, is 2-9).
#define GPS_LINE_TRAFFIC_FWD  2
#define GPS_LINE_TRAFFIC_BACK 6
#define GPS_LANES_PER_SIDE    4

int   gps_num_routes(void);
int   gps_num_points(void);
int   gps_route_first(int r);
int   gps_route_points(int r);
// TrackRoute +0x60 / +0x68: the box round the route's points.
void  gps_route_bbox(int r, float mn[2], float mx[2]);
int   gps_point_route(int p);
// TrackRoute +0x82 + line: whether the route has driving line `line`.
bool  gps_route_has_line(int r, int line);
// HasTrafficLaneData (0x801F8A58): any traffic lane on the side `fwd` says.
bool  gps_route_has_traffic(int r, bool fwd);

void  gps_point_pos(int p, float out[3]);
void  gps_point_fwd(int p, float d[2]);   // GetForwardDirection
void  gps_point_back(int p, float d[2]);  // GetBackwardDirection
float gps_point_len(int p);               // GetLength, metres to the next point
float gps_point_len_back(int p);          // the length behind it
float gps_point_half(int p);              // the wider of the two half-widths
// The road's two half-widths at the point, +0x14 and +0x16: its left and right
// edges, metres either side of the route's own direction.
void  gps_point_halves(int p, float* left, float* right);
u32   gps_point_bits(int p);              // +0x0C
// +0x08: the point's distance round the race, m, the TrackPathManager's
// measure the cars' TrackPathCoordinates (Car +0x8EC) are in. 0 throughout
// RoutesFreeRoam.bin.
float gps_point_race_dist(int p);
// TrackRoute +0x70, what GetBits / GetBool read: bit 1 the route runs against
// the race, bit 6 IsRelevant's, bit 9 its points' race distances are not to be
// used (DistanceAroundTrackIsValid 0x801F4E28).
u32   gps_route_bits(int r);
// GetRelativePoint: the next point the way `fwd` faces, -1 at the route's end.
int   gps_point_next(int p, bool fwd);
// GetForwardsDrivingLineOffset (0x801F846C): line `line`'s offset across the
// road at the point, metres to the right of the route's own direction.
float gps_line_offset(int p, int line);
// The offset as the lane's own traffic sees it: metres to the right of the way
// it drives, which is the point's own s8 for either side
// (GetNearestTrafficLane 0x80018650 measures a backward lane against the
// backward direction).
float gps_lane_offset(int p, int line);

// TrackRoutePoint +0x2C + line: the line's speed, m/s.
float gps_lane_speed(int p, int line);
// GetPostedForTrafficForward / Backward (0x803024B0 / 0x803024DC): bit 25 /
// 26 of the point's bits, whether traffic may be put down there that way.
bool  gps_point_posted_traffic(int p, bool fwd);
// IsPostedAtAll / IsPostedAtAllBackwards (0x801F839C / 0x801F8404): posted
// that way for any of ePostednessType's three -- racers (bit 11 / 18),
// traffic (25 / 26) and the player (27 / 28).
bool  gps_point_posted_at_all(int p, bool fwd);
// NumSignposts (r13-0x62B8): the signposts in the network.
int   gps_num_signs(void);

int   gps_point_num_signs(int p);
int   gps_point_sign(int p, int i);
u32   gps_sign_bits(int s);    // +0x0C
// The point a taken signpost lands on (GetNewRoutePoint), -1 if it does not
// resolve.
int   gps_sign_to(int s);

// The traffic intersections (chunk 0x34123): where two routes cross and
// traffic on one gives way to the other (TrafficIntersection; its state, the
// baton and the cars coming, is traffic.c's). An index in the chunk.
int   gps_num_intersections(void);
// GetTrafficIntersection (0x801F7BF4): point `p`'s (bits 19-24), -1 for none
// or for one IsRelevant (0x8002C1C8) turns down.
int   gps_point_intersection(int p);
// +0x00: 1 the second route gives way, 2 it stops first, 3 both stop.
int   gps_intersection_type(int i);
// GetIntersectionRoute (0x8002C174): which of its two routes route `r` is, 0
// or 1; -1 for neither.
int   gps_intersection_side(int i, int r);
// GetLength (0x8002B930): how far before the crossing point on route `side`,
// driving the way `fwd` says, the other road's near edge is, m.
float gps_intersection_length(int i, int side, bool fwd);
