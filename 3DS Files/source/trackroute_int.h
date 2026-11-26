#pragma once
#include "trackroute.h"

// The TrackRouteSet's own layout, for trackroute.c and futurepoints.c
// (TASKS.md #549). Nothing else includes this.

// TrackRouteManager::Loader's stride is 0xE08C - (0x400 - n) * 0x38, so a
// route never holds more than 0x400 points.
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
	// What FuturePoints reads (TASKS.md #549, futurepoints.c):
	float len;            // +0x10 u16 / 256: metres to the next point
	                      // (GetLength 0x801F7E28's TrackRoutePoint +0x10)
	u16 sign[2];          // +0x1C, its two Signpost slots, 0xFFFF for none
	                      // (GetNumSignposts 0x801F7AE8, GetSignpost 0x801F7B2C)
	u8 speedB;            // +0x2D, line 1's speed: the racing line driven
	                      // up the route (FuturePointBackwards 0x8004486C)
	float slope;          // +0x21 s8 / 128: rise a metre to the next point
	u8 tInt;              // bits 19-24 of +0x0C, the traffic intersection
	                      // number, 0x3F for none (FarEnoughFromPrevPoint)
} TRPoint;

// A route's header, as much of it as trackroute_nearest reads.
typedef struct {
	int first, count;           // its points in pts
	float bbMin[2], bbMax[2];   // +0x60 / +0x68
	u32 bits;                   // +0x70, TrackRoute::GetBits (0x80302548)
	u8 line[10];                // +0x82 + line: non-zero where it has the line
	int num;                    // its number, its points' bits 0-10 (GetRoute 0x801F7C60)
	u32 rbits;                  // +0x70 again, for GetBool(10): racers keep off
	                            // (DecideSignpostAsRacer 0x8000A914)
	u8 lineHas[2][2];           // +0x74 + line * 8 and +0x75: FillUpArray's
	                            // ReduceFixup asks either for racing line 0 / 1
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

// The set's route with number `num` (a Signpost's newRn / oldRn), -1 for none.
int trackroute_route_by_num(const TrackRouteSet* s, int num);
