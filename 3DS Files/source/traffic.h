#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "aidriver.h"
#include "cardb.h"
#include "carlights.h"
#include "carsim.h"
#include "model3d.h"
#include "race.h"
#include "trackdb.h"
#include "trackpath.h"

// Traffic (TASKS.md #316): the civilian cars on the city's roads, in free roam
// and in a race.
//
// Retail runs traffic as three objects, each ported here from Speed.elf:
//
//   TrafficTeleporter  RaceStarter::AddTrafficCars (0x80119E20) gives it the
//                      cars; PlaceNewCarOntoRaceTrack (0x8002D0B0) puts each
//                      one down at random over the whole road network, and
//                      DoTimestep (0x8002C8B0) then thinks once a second and
//                      moves ONE car it may teleport (stopped, or out of the
//                      player's sight) to a road 250 m or more up or down the
//                      road from the player (TryToTeleportCarOntoRaceTrack
//                      0x8002E9BC), at the spacing the player's
//                      TPZ_TRAFFIC_TYPE zone asks for.
//   TrafficController  Think (0x8002A8EC) every 0.2 s: the lane it drives
//                      down the RoutesFreeRoam.bin graph (FuturePoints, with
//                      DecideSignpostAsTraffic at each junction) and the speed
//                      it holds (SetDesiredSpeedAsTraffic 0x8000B2D8), which
//                      a PsychoDriver (aidriver.h, movement index 3) turns
//                      into a steering angle and the pedals. Where two routes
//                      cross (TrafficIntersection, gps.h), the car on the
//                      second gives way to what is coming down the first
//                      (AIActionObeyTrafficIntersections).
//   TrafficMover       Move (0x8002A080): no physics -- the car rolls along
//                      a circle off its steering angle, its speed off two
//                      four-sample tables, and Car::PutFlatOnTrack lays it on
//                      the ground (carsim_traffic_pose). A car something hits
//                      goes on the MellowMover (carsim_set_mellow,
//                      SetToPhysicsMode 0x8002B698) until it is teleported.
//
// The types and skins are StreamingTrafficCarManager's: SetInitialDesired-
// CarTypes (0x8003C760) picks at most four car types by the weights the
// player's traffic zone gives them, and each car draws a skin
// (CarTypeInfo::GetRandomSkin 0x802441D4). The packs are tools/car_pack.py's
// CARS/<CAR>.nfsc, which carry every <CAR>_SKINn; a skin is the model's own
// textures with SKIN1's slot swapped for the one drawn.
//
// A traffic car is drawn as every car in the world is: each batch lit by the
// light material it is authored with (TRAFFIC, TRAFFICWINDOWS, MOLDINGS, ...;
// model3d_set_light_mats), with the light flares CreateCarLightFlares makes
// of its body's head- and brake-light markers. Both come from the pack's TRFX
// block. Its DriverInfo is type 3 (AddTrafficCars), so UpdateHeadlightsAndHorn
// keeps its lamps on and UpdateBrakelightsState lights its brake lamps off its
// driver's brake (+0x1F8, traffic_brake()), with no hold.
//
// In a race the same three objects run over the race's own road network,
// Routes<track><F|B>.bin (gps_race_load), with the race's traffic level
// (TheRaceParameters +0x08: the career event's +0x36, Quick Race's setting)
// picking the count, and the teleporter measures by distance round the race
// (TrackRoutePoint +0x08 against each car's TrackPathCoordinate, Car +0x8EC)
// wherever a point carries one: what is in sight, what is too close, which
// cars are stalled. Before the race starts no car is put down within the
// track's +0xF8 metres past the start line, nor behind it (CanPlaceCarHere).
// A route that runs against the race (TrackRoute bit 1) has its sides swapped,
// so "forward" always means the race's way.
//
// TryToTeleportCarOntoDragTrack is ported (teleportOntoDrag). A corner's cubic bends only the racing line, which traffic does not
// drive, so a traffic lane is cut straight across it (traffic.c cornerCut), as
// on the GameCube. Cross traffic has nothing to port: no route file on the PC
// disc carries an emitter (chunk 0x34124). Nor have the model streaming swaps
// (StreamingTrafficCarManager::Update, every 5 s): they load and unload only
// types no car is using, and a car's type is set once (ChangeCarType's two
// callers are the Car constructor and FinishedLoadingAllRideInfos), so the
// cars on the road never change model.

#define TRAFFIC_MAX       16   // AddTrafficCars' min(16, ...)
#define TRAFFIC_TYPES     4    // SetInitialDesiredCarTypes' distinct types
#define TRAFFIC_FAIL_WHYS 7    // traffic.c's TF_* teleport failure reasons
#define TRAFFIC_CANDIDATES 16  // the traffic records (kind 2) in the car table
#define TRAFFIC_NODES     24   // the lane walker's points ahead of the car

// AIGoalBeTrafficCar's actions, each with the score ChooseNewAction
// (0x8000CC34) weighs it at: AIActionDriveTrafficLane (0),
// AIActionAvoidAvoidableAsTraffic (300) and AIActionObeyTrafficIntersections
// (600).
enum { TRAFFIC_ACT_LANE, TRAFFIC_ACT_AVOID, TRAFFIC_ACT_OBEY };

// One point of a car's lane down the road: FuturePoints' role.
typedef struct {
	int   point;      // the TrackRoutePoint
	bool  fwd;        // the lane runs the route's own way
	int   line;       // the driving line, 2..9
	float pos[3];     // the lane's position at the point
	float s;          // metres along the walk
	float speed;      // the lane's speed there (TrackRoutePoint +0x2C), m/s
	float localD;     // metres on from the point the way it runs (FuturePoint +0x54): a corner's
	float cap;        // a corner's speed cap here (FinishInterpolatingCorners), m/s
} TrafficNode;

typedef struct {
	int       rec;          // the CarDb record
	CarRec    car;          // a copy of it: the sim's data and the skins
	Model3D*  model;
	int       skin1Slot;    // the model's slot holding <CAR>_SKIN1, -1 if none
	// One texture array per skin number (index n - 1), the model's own
	// textures with SKIN1's slot swapped; NULL where the pack lacks it.
	C3D_Tex** skinTex[CARDB_MAX_SKINS];
	CarLights lights;       // the flares, off the pack's TRFX block
	bool      lit;          // the batches' light materials took
	int       weight;       // +0x854[zone type]: how many may be out at once
	int       count;        // how many are
} TrafficType;

typedef struct {
	bool  used;        // has a type and a sim
	bool  active;      // on the road and drawn (AICarController::DisableCar)
	bool  stopped;     // TrafficController +0x784: teleportable at once
	bool  stopDriving; // StopDriving: a hard hit, the pedals held
	unsigned char stopWhy; // PORT, for the log: 1 a hit, 2 a stop sign, 3 DbgTrafficBehind
	bool  hideActive;  // traffic_hide_all: on the road when the cars were hidden
	int   type;        // index into Traffic.type
	int   skin;        // 1-based <CAR>_SKINn
	C3D_Tex** tex;     // the skin's texture array, NULL for the model's own

	CarSim   sim;
	AiDriver drv;

	// TrafficMover (+0x0C/+0x10 position, +0x14 heading, +0x18 speed):
	// where the car is, which way it faces (rad) and how fast it rolls. The
	// mover is not used while `mellow` is set.
	float x, y, heading, speed;
	bool  mellow;      // on the MellowMover after a hit
	float hitSum;      // the hit's impulses summed (Collision +0x48)
	float hitLast;     // seconds since the last one

	// The lane, walked ahead of the car.
	TrafficNode node[TRAFFIC_NODES];
	int   nNodes;
	int   cur;         // the node the car is past, toward cur + 1
	bool  walkEnded;   // a dead end or a route's end: no more nodes
	int   walkPoint;   // where the walk has got to, which may be past the last
	bool  walkFwd;     //   node (FarEnoughFromPrevPoint drops points)
	float cornerS;     // the walk's metres at the last corner it took (+0x358)
	int   lane;        // 2..5, the lane on its side of the road
	float progress;    // metres along the walk the car has reached
	float roadZ;       // the lane's height under the car

	float desired;     // SetDesiredSpeedAsTraffic's answer, m/s
	float thinkSec;    // until the next Think (0.2 s)
	// The drive target's L (DrivingChoice::FillUpDriveTarget 0x8001D5C0): at
	// the last Think the car was `driveL` metres right of its lane, `driveS`
	// along the walk; the target eases back onto the lane from there.
	float driveL, driveS;
	float playerDist;  // to the player, m, as of this frame
	RaceCoord coord;   // in a race: the car's TrackPathCoordinate (Car +0x8C8)

	// AIGoalBeTrafficCar's current action, a TRAFFIC_ACT_*.
	int   action;

	// AIActionAvoidAvoidableAsTraffic (+0x1FC who, +0x208 time, +0x20C the
	// look-ahead): who is avoided -- a car index, TRAFFIC_MAX for the player.
	int   avoidWho;
	float avoidTime;
	float avoidLook;

	// AIActionObeyTrafficIntersections (+0x1FC the intersection, +0x200 it
	// has stopped there).
	int   obeyInter;
	bool  obeyStopped;
	int   firstInter;  // the walk's first intersection ahead, -1 for none
	int   firstSide;   //   which of its routes the car is on
	float firstDist;   //   GetDistanceToFirstTrafficIntersection, m

	// PORT, a diagnostic (#401): over each 8 s window, how far the car turned
	// and how far it got from where it was; a full turn inside 20 m is a
	// "donut" and is logged once.
	float dnTurn, dnPrevH, dnProg0, dnSec, dnX, dnY;
	bool  dnLogged;
} TrafficCar;

// A TrafficIntersection's state (+0x08 on): who has the right of way, and
// which car is coming down each route and when.
typedef struct {
	s8  baton;        // +0x10: the route through, -1 for neither
	s8  soonest[2];   // +0x08: each route's car with the least ETA, -1 none
	struct {
		bool  in;     // +0x14: coming to it
		s8    side;   // +0x18: down which route
		float dist;   // +0x1C: to its stopping zone, m
		float eta;    // +0x20: s
	} car[TRAFFIC_MAX];
} TrafficInter;

typedef struct {
	bool  open;
	char  dataRoot[256];
	TrafficCar  car[TRAFFIC_MAX];
	int   count;            // AddTrafficCars' count
	TrafficType type[TRAFFIC_TYPES];
	int   nTypes;

	// The candidates: every traffic record with a pack on the card.
	int   cand[TRAFFIC_CANDIDATES];
	int   nCand;

	const TrackPaths* paths;
	float initialSpacing;   // TrackInfo +0x100 + reversed * 4
	u32   rng;              // bRandom's state, the traffic's own (PORT)

	// TheRaceParameters +0x08, the traffic level 1..3 (3 in free roam), and
	// what the track's TrackInfo says for it and the race's direction.
	int   level;
	bool  race;             // on the race's road network, by its distances
	bool  drag;             // a drag: PlaceNewCarOntoDragTrack / TryToTeleportCarOntoDragTrack
	float oncoming;         // +0x108[level]: the share sent against the race
	bool  startOk;          // +0xF4[reversed]: traffic allowed by the start line
	float startClear;       // +0xF8[reversed], m
	const struct TrafficRace* rc; // this frame's, during traffic_update
	// The player's car is blinking after a reset (Car::IsBlinking): it passes
	// through the traffic as through every other car (#484). Set by the
	// caller before traffic_update.
	bool  playerGhost;

	// GenerateRandomPoint's draw: per route, the points before it on routes
	// with traffic lanes forward / backward (nRoutes + 1 each).
	int*  cumFwd;
	int*  cumBack;
	int   nRoutes, totalFwd, totalBack;

	// One per gps intersection (gps_num_intersections).
	TrafficInter* inter;
	int   nInter;
	int   yields;           // cars Obey has held at a stop line, for the log
	int   laneChanges;      // lanes the avoid has moved cars to, for the log
	int   racerHits;        // traffic_car_contact touches, for the log
	int   cornersCut;       // corners the walks have cut, and taken straight, for the log
	int   cornersStraight;
	float cornerOld, cornerNew, cornerOn;  // the last cut: its nodes from the new road's edge

	// TrafficTeleporter
	bool  placed;           // AddTrafficCars has run
	bool  hidden;           // +0x08 cleared: MakeAllTrafficCarsDisappear, the teleporter off
	float dbgBehindM;       // TrafficOpen's, a diagnostic
	bool  dbgPlaced;        //   car 0 is down
	int   dbgTries;         //   attempts so far, a second apart
	float dbgTrySec;        //   until the next
	float thinkSec;         // until its next think (1.0 s)
	int   lastCar;          // the round robin's last car
	int   altStart;         // +0xC: the alternate starting point the last teleport took
	int   altStarts;        // teleports tried from another route's point, for the log
	int   zoneType;         // the player's TPZ_TRAFFIC_TYPE zone +0x3C, -1 none
	int   teleports, teleportFails;
	int   failWhy[TRAFFIC_FAIL_WHYS];  // the failures by traffic.c's TF_* reason, for the log
	float logSec;
} Traffic;

// What traffic_open needs. `cars` and `track` are read during the call only.
typedef struct {
	const char* dataRoot;      // the game data directory, e.g. "sdmc:/nfsu2data"
	const CarDb* cars;         // GLOBAL/GLOBALB.BUN's car table
	const TrackRec* track;     // the count and spacing: 4000 in free roam, else the race's
	int level;                 // TheRaceParameters +0x08: 3 in free roam, 0 = no traffic
	bool reversed;             // TheRaceParameters +0x04
	bool race;                 // a race: gps_race_load()'s network, by race distance
	bool drag;                 // TheRaceParameters +0x40 / g_tweakIsDragRace: a drag
	const TrackPaths* paths;   // the TPZ_TRAFFIC_TYPE zones; may be NULL
	const float* playerPos;    // where the player is, for the zone's types
	u32 seed;
	void (*log)(const char* msg);
	float dbgBehindM;          // DbgTrafficBehind: car 0 this far behind the player (< 0 ahead, parked), 0 = off
} TrafficOpen;

// Builds the traffic: the types for the player's zone, their models and
// skins, and one sim per car. The cars are put on the road by the first
// traffic_update. Needs gps_load() first (the road network), or for a race
// gps_race_load(). False and nothing open when there is no road network, no
// traffic record or no pack, or when the level asks for no cars.
bool traffic_open(Traffic* t, const TrafficOpen* o);
void traffic_close(Traffic* t);

// What a race tells its traffic each frame. In free roam traffic_update takes
// NULL.
#define TRAFFIC_RACERS 8
typedef struct TrafficRace {
	const RaceRoute* route;   // TheTrackPathManager: CompareDistances, the cars' coordinates
	int   stage;              // pCurrentRace +0x00; the teleporter runs at RACE_STAGE_RACING
	bool  started;            // +0x04, the start time, is set and reached
	float startDist;          // the start line, m round: length * +0xBF8[+0xBF6] / 100
	float playerDist;         // the player's Car +0x8EC
	bool  playerWrongWay;     // Player +0x234 (DetectWrongWay)
	// The AI racers: TooCloseToAnotherCar walks every car in the world.
	int   nRacers;
	float racerPos[TRAFFIC_RACERS][2];
	float racerDist[TRAFFIC_RACERS];
} TrafficRace;

// One frame: the teleporter, each car's Think and its mover. `player` is the
// player's car, already stepped this frame; it is pushed by any traffic car
// it touches. `w` is the world the cars stand on. `race` is NULL in free roam.
// `log` (may be NULL) gets a line every 10 s.
void traffic_update(Traffic* t, const CarSimWorld* w, CarSim* player,
	const TrafficRace* race, float dt, void (*log)(const char* msg));

// A race's AI car `car` against every traffic car near it, once both have
// been stepped this frame: World::DoTimestepCollisions runs every pair of
// bodies. The traffic car goes on its MellowMover as for any hit; the racer
// only takes the push, as AICarController::SetToPhysicsMode (0x80008930)
// switches it to physics only for the player's car (the other Car's +0x18).
void traffic_car_contact(Traffic* t, CarSim* car, float dt);

// TrafficTeleporter::MakeAllTrafficCarsDisappear (SpeedPS.elf 0x8002FE68) and
// RecycleAllTrafficCars (0x8002FDB4) when `hide`: every car off the road and
// marked teleportable at once, the teleporter stopped. Else
// MakeAllTrafficCarsReappear (0x8002FEF4): every car back where it was and
// the teleporter on again, to move the recycled cars as it thinks. The photo
// shoot's (ShowcaseMagazineDialog).
void traffic_hide_all(Traffic* t, bool hide);

// GuessTargetDestinations (0x800174E8) asked for one target, as the
// teleporter and RandomEncounterManager::TryToPlaceInactiveCars ask: from
// route point `*p` going `*fwd`, `metres` on, written back. `rng` draws a
// junction's coin; `racing` is TheTrackPathManager +0x2C. Needs gps_load().
bool traffic_guess_target(u32* rng, bool racing, int* p, bool* fwd, float metres);
// Car::GetRoutePoint: the free-roam route point nearest `car`, and whether it
// is going the route's way there. -1 for none.
int traffic_route_point(const CarSim* car, bool* fwd);

// Whether traffic car `i` is to be drawn.
static inline bool traffic_drawn(const Traffic* t, int i) {
	return t && t->open && i >= 0 && i < t->count && t->car[i].used && t->car[i].active &&
		t->type[t->car[i].type].model != NULL;
}

// The model a car is drawn with, and the texture array to hand
// model3d_set_textures() before drawing it (NULL: the model's own).
static inline Model3D* traffic_model(const Traffic* t, int i) {
	return t->type[t->car[i].type].model;
}
static inline C3D_Tex* const* traffic_textures(const Traffic* t, int i) {
	return (C3D_Tex* const*)t->car[i].tex;
}
// Its flares, and its driver's brake (0..1).
static inline const CarLights* traffic_lights(const Traffic* t, int i) {
	return &t->type[t->car[i].type].lights;
}
static inline float traffic_brake(const Traffic* t, int i) {
	return t->car[i].drv.prevBrake;
}
// No wheel down while its physics runs (a hit car near the player, #394); a
// car on the TrafficMover is posed on the road and never in the air.
static inline bool traffic_airborne(const Traffic* t, int i) {
	return t->car[i].mellow && !t->car[i].sim.onGround;
}
