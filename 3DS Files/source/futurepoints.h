#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "carsim.h"
#include "trackroute.h"

// Retail's FuturePoints (TASKS.md #549): the racing line a racer actually
// drives, rebuilt as it goes from the TrackRoutes ahead of it.
//
// A racer in Speed.elf keeps a current TrackRoutePoint and walks the routes
// from it -- point by point, and across Signposts where one route meets
// another -- laying down up to 40 FuturePoints ahead of the car
// (FuturePoints::Update 0x80033FD0). Each carries a lateral fixup off the
// route's own racing line: eased back to it between signs (ReduceFixup), bent
// round corner signs with a cut to an apex (InterpolateFuturePointsForCorner,
// FinishInterpolatingCorners, InterpolateSplineDrivingLinesForCorner) and
// splined through the 8 m points between real ones
// (InterpolateDrivingLinesBetweenFuturePointsUsingSplines). Each also carries
// a speed: the route point's own driving-line speed, capped at each corner
// by the curvature through its apex and scrubbed for braking and
// acceleration (FillInDrivingLineSpeeds, ScrubSpeeds).
//
// DrivingChoice::BuildDriveTarget (0x8001CFBC) then turns them into the
// points the driver steers at: from the car's own lateral, eased onto the
// choice's lateral at 0.151 m a metre, kept 1 m plus half the car inside the
// road (FillUpDriveTarget 0x8001D500, ChangeCurrentL 0x8001DADC).
//
// tools/re/wip549fp/SPEC.md has the decode, fp_all_ps.c and fpoint_ps.c the
// functions themselves.
//
// PORT: racers only -- driving line 0 (1 up a route), not the four traffic
// lanes. Where retail reads a lane it is lane 0's road edges (the corner's
// width); the lanes' own lines and speeds are not kept.

typedef struct FuturePoints FuturePoints;

// What the car plans on: ScrubSpeeds' braking (GetCarBraking) and
// acceleration table (RidePhysicsInfo, CARSIM_ACCEL_N samples over
// [0, accelTop]; accelTop 0 for none), and the friction a corner's speed is
// planned on (GetCarStaticFriction).
typedef struct {
	float friction, brake;
	float accel[CARSIM_ACCEL_N];
	float accelTop;
} FPCar;

// A fork's decision (DecideSignpostAsRacer 0x8000A914's random arm): sign
// `sign` of the set, a type-4 sign onto route number `newRoute`, taken at
// odds `p`. Return whether to take it. NULL: never.
typedef bool (*FPForkFn)(void* ctx, int sign, int newRoute, float p);

FuturePoints* fp_new(const TrackRouteSet* s, FPForkFn fork, void* forkCtx);
void fp_free(FuturePoints* f);

// PORT (TASKS.md #549): the race's own way over the routes. Retail's racers
// keep to the race at its Signposts by the race's postedness, which the port
// has not decoded, and some of a race file's signs are out of its table (a
// route point's slot past the file's count, which retail skips too). So the
// walk follows `hops` where the race changes route, and at a sign leaving a
// route the race is on (`onRace[route]`, the set's nRoutes) for one it is not,
// takes it only as one of the race's forks (the fork callback). Both are the
// caller's and must outlive `f`; NULL for none.
void fp_set_race(FuturePoints* f, const TrackRouteHop* hops, int nHops, const u8* onRace,
	bool loop);

// Forgets the points (Clear 0x800338C0) and the sign decisions.
void fp_clear(FuturePoints* f);

// Puts the walk on the racer-posted point nearest `pos`, the way `heading`
// points (PORT: retail's controller already holds its route point). False for
// none within reach.
// PORT: `preferRoute` (an index into the set, -1 for none) is the race's own
// route where the car is: a divided road's other carriageway, driven the other
// way, is as near and heads the same way (#549, 4104F).
bool fp_seed(FuturePoints* f, const float pos[3], const float heading[2], int preferRoute,
	int nextHop);

// Update (0x80033FD0) for a car at `pos`: drops the points behind it, walks
// on until `maxD` metres past it (at least 10 points, at most 40). False when
// there are none (not seeded, or the walk is lost).
bool fp_update(FuturePoints* f, const float pos[3], float maxD, const FPCar* car);

int fp_count(const FuturePoints* f);
// How far `pos` is from the nearest point's racing line; 1e30 for none.
float fp_distance(const FuturePoints* f, const float pos[2]);

// BuildDriveTarget: up to `max` points to steer at (world x, y), from the
// one ahead of the car, at lateral `targetL` off the racing line, moving
// toward it at `rate` m a metre, `margin` metres inside the road. The drive
// target is kept from frame to frame as retail's is. Returns the count.
int fp_drive_target(FuturePoints* f, const float pos[3], float speed, float targetL,
	float rate, float margin, float out[][2], int max);
void fp_drive_target_clear(FuturePoints* f);
// PORT (TASKS.md #549): move the drive target's point `i` to `to` (the port's
// road edges), so the next frame's target carries on from where it was put.
void fp_drive_target_move(FuturePoints* f, int i, const float to[2]);
// DriveTarget::GetLateralOffset: how far right of its drive target the car at
// `pos` is, against the target's first point.
float fp_drive_target_offset(const FuturePoints* f, const float pos[2]);

// InterpolateDrivingLineSpeed (0x800386AC): the racing line's speed (m/s)
// `ahead` metres along it past the car's point; -1 for none.
float fp_speed_ahead(const FuturePoints* f, const float pos[2], float ahead);

// The racing line's lateral of `pos` (+ right) at the point nearest it.
float fp_lateral(const FuturePoints* f, const float pos[2]);
// The world point `L` right of the racing line `ahead` metres past `pos`.
bool fp_point_ahead(const FuturePoints* f, const float pos[2], float ahead, float L,
	float out[2]);
