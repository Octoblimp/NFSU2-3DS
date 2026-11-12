// The car's opening parts: the hood, trunk and door swings the customisation
// shops play (TASKS.md #372 step 7). Retail's CarAnimManager /
// CAnimCarPartsScene (SpeedPS.elf 0x80230870 AnimateCarPart, 0x80052a70
// TriggerCarPartAnimation, 0x800537e4 GetCarPartAnimMatrix) over the
// per-car PARTS_ANIMATIONS.bin, packed by tools/carpartanim_pack.py into
// CARS/<CAR>.nfsa. carkit.c tags the batches each location moves; this works
// out the matrices model3d_set_car_parts() takes.
#pragma once

#include <stdbool.h>
#include "model3d.h"

// CarAnimLocation.
enum { CARANIM_HOOD = 0, CARANIM_TRUNK = 1, CARANIM_DOOR_LEFT = 2, CARANIM_DOOR_RIGHT = 3 };
// CarAnimState: what GetLocationState reports (closed when no animation was
// ever started there).
enum { CARANIM_OPEN = 0, CARANIM_CLOSED = 1, CARANIM_OPENING = 2, CARANIM_CLOSING = 3 };

// Loads CARS/<car>.nfsa under dataRoot and shuts every part. False (and
// every part shut for good) when the car has none.
bool carpartanim_load(const char* dataRoot, const char* car);
void carpartanim_unload(void);
bool carpartanim_loaded(void);

// The styles AnimateCarPart reads off the car's parts when a part moves:
// hoodStyle 0 (one piece), 2 (split left / right: hood kit 5) or 4 (split
// front / back: kit 5 with VERTSPLIT); hood22 the _STYLE22_HOOD(_CF) part,
// whose halves run at 0.75 and 1.25; doorStyle the door part's ANIMSTYLE
// (0 regular, 1 scissor, 2 suicide), or 2 + the widebody kit for a scissor
// door on a widebody (the WIDE1..4 swings).
void carpartanim_set_styles(int hoodStyle, bool hood22, int doorStyle);

// AnimateCarPart's OpenPart / ClosePart: starts the location opening or
// closing (retail's sound is the caller's). Ignored when it is already
// going that way or there; reversing mid-swing keeps the position.
void carpartanim_trigger(int loc, bool open);
// The same at a speed (AnimateCarPart's float; carpartanim_trigger's is 1):
// FEDoCarPartAnimNow's 20 on a magazine cover runs the swing in a few frames.
void carpartanim_trigger_speed(int loc, bool open, float speed);
int carpartanim_state(int loc);
// The animation length of a location's current swing, in seconds.
float carpartanim_length(int loc);
// KillDoorAndSplitHoodAnimations (0x80051e14): the doors, and a split hood,
// snap shut.
void carpartanim_kill_doors_and_split_hood(void);
// Every part shut at once (a new car, leaving the shop).
void carpartanim_reset(void);

// The pose for the next car draw (Render__13CarRenderInfo's per-model
// GetAnimationMatrixAtTime). Advances the states as GetCarPartAnimMatrix
// does. fe = a frontend draw.
void carpartanim_pose(Model3DPartPose* p, bool fe);
