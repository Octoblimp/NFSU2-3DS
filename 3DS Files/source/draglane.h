#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "trackroute.h"

// A drag's lanes (TASKS.md #268 D step 6). No car in a drag drives a racing
// line. The player's car is held in a traffic lane by its DragController and
// moved a lane at a time; the racers' AIGoalWinDragRace drives
// AIActionDriveTrafficLane and changes lanes only to get round a car
// (DrivingChoice::ChangeTrafficLane). Both read the road's traffic lanes,
// lines 2..5 the way the race drives (trackroute.h's TrackLanes).

typedef struct {
	const TrackLanes* lanes;   // NULL: not a drag, or a road with no lanes
	// TheRaceParameters' track (+0x8a), which picks an InvalidLaneChange
	// table, and the race's eTrackDirection (DAT_803fca44): 0 forward, 1
	// backward.
	int track;
	int dir;
} DragLanes;

// SetLanesToNotLockOnto's "none" (0xff either way).
#define DRAGLANE_NONE 0xff

// GetNearestTrafficLane (0x80018650): the lane at `dist` whose lateral is
// nearest `lat` (metres right of the line), leaving out lanes exLo..exHi,
// DRAGLANE_NONE either for none left out; -1 for none. Only the lanes at the
// car's height `z` count: retail's are its own route's.
int draglane_nearest(const DragLanes* d, float dist, float lat, float z, int exLo, int exHi);

// FuturePoint::CanChangeDrivingLine (0x8003AF48) from one traffic lane to
// another at `dist`, the car `raceDist` into the race. Each lane on the way
// must be on the same road as the one before it. `ai` is DriverInfo type
// 2, which meets a flagged ban 80 m sooner; `drag` is the player's car under
// its DragController (control mode 2), for which any lane runs far enough
// (DoesDrivingLineExtendAllTheWayForCar).
bool draglane_can_change(const DragLanes* d, float dist, float raceDist, int from, int to,
	bool ai, bool drag);

// DragController::GetDesiredLateralSpeed (0x8001ECB0): DragLateralSpeedGraph
// at the car's speed (m/s), metres across a metre along.
float draglane_lateral_speed(float speed);

// DragController (0x8001E918 on), the player's car in a drag, and the
// DragDriver's JoyHandler_ChangeLane (0x80022EBC) that steers it.
typedef struct {
	int state;            // +0x7C0: 0 moving left, 1 moving right, 2 locked on
	int exLo, exHi;       // +0x7C4 / +0x7C8, the lanes not to lock onto
	bool changing;        // +0x7CC
	float changeAt;       // +0x7D8, the race distance the last change began at
	bool leftHeld, rightHeld;   // DragDriver +0x744 / +0x740
	// SetNeedToThrowOutDriveTarget: the caller's drive target starts afresh.
	bool rethrow;
	// The DrivingChoice: the lane the car makes for (-1 before the first) and
	// how fast across, metres a metre (SetChoice's L is 0: the lane itself).
	int lane;
	float rate;
} DragController;

void dragctl_init(DragController* c);

// One frame of the lane buttons: the change-lane events' held state, which
// the handler acts on as it changes.
void dragctl_input(DragController* c, bool left, bool right);

// DragController::Think: the car `dist` along the line, `lat` right of it and
// `z` high, at `speed` m/s and `raceDist` into the race.
void dragctl_think(DragController* c, const DragLanes* d, float dist, float lat, float z,
	float speed, float raceDist);

// AIActionHitWallDragRace for the DragController's car: the exclusion goes and
// the choice is the nearest lane.
void dragctl_hit_wall(DragController* c, const DragLanes* d, float dist, float lat, float z,
	float speed);

// AIActionIJustGotHitDragRace's CalculatePointsToDrive for the DragController's
// car (opponent_player_drag_hit): SetLanesToNotLockOnto(0xff, 0xff), +0x7CC
// cleared, and the choice `lane` (-1 to keep it).
void dragctl_hit_car(DragController* c, int lane);
