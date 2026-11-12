// The career car built out of the parts its save picked (TASKS.md #204).
//
// tools/car_pack.py's .nfsc is one fixed bake of a car's KIT00 parts -- the
// showroom car. A career save names its own: a body kit, a hood, a spoiler out
// of the shared SPOILER library, a roof scoop, mirrors, exhaust tips, a plate,
// rims (with spinners) and brakes. tools/kit_pack.py
// ships those as part kits (CARS/<KIT>.nfsk) and a part -> model table
// (GLOBAL/PARTMODELS.nfsm); this puts the save's picks together into one
// ordinary .nfsc in memory and hands it to model3d_from_memory(), so drawing
// it is exactly the path every other car already takes.
//
// What is assembled, and where, is Render__13CarRenderInfo's own rule set
// (SPEED2.EXE 0x6219xx; see the comment above main_loop_draws() in carkit.c and
// TASKS.md #204 for the decode). Anything the port has not decoded yet is left
// out rather than approximated. A rim or brake whose kit is not on the card
// falls back to the car's own stock one.
#pragma once

#include <3ds.h>
#include <stdbool.h>
#include "carskin.h"
#include "model3d.h"

// The paint slots a batch can carry in flags bits 24-27 -- Render's
// per-component materials, [0xe1] for the body and [0xe4..0xea] beside it.
enum {
	CARKIT_PAINT_BODY = 0,
	CARKIT_PAINT_SPOILER,
	CARKIT_PAINT_ROOF,
	CARKIT_PAINT_MIRROR,
	CARKIT_PAINT_RIM,
	CARKIT_PAINT_EXHAUST,
	CARKIT_PAINT_BRAKE,
	CARKIT_PAINT_AUDIO,
	CARKIT_PAINT_SLOTS
};

// One underglow tube: the two car-space endpoints of a
// FRONT_NEON_<n>/SIDE_NEON_<n>/REAR_NEON_<n> marker pair. carneon.c stretches
// a quad between them.
typedef struct {
	float a[3];
	float b[3];
} CarKitNeonSeg;

// Three front, two side and one rear is what every stock car carries; the
// wider kits add no more, and retail's own loop stops after three of each
// prefix, so nine is the ceiling it can reach.
#define CARKIT_NEON_SEGMENTS 9
// The constructor's own loop bounds for the engine and trunk tubes.
#define CARKIT_ENGINE_NEON 5
#define CARKIT_TRUNK_NEON  4

// One light flare (TASKS.md #296): a LEFT_, RIGHT_ or CENTRE_ HEADLIGHT or
// BRAKELIGHT marker off model 0 of one of the car's slots, which
// CreateCarLightFlares (Speed.elf 0x80235e50) makes one flare of. The
// HEADLIGHT and BRAKELIGHT parts carry them, so a fitted aftermarket lamp
// moves its flares with it. carlights.c draws them.
enum { CARKIT_LIGHT_HEAD = 0, CARKIT_LIGHT_BRAKE = 1 };
enum { CARKIT_LIGHT_LEFT = 0, CARKIT_LIGHT_RIGHT, CARKIT_LIGHT_CENTRE };

typedef struct {
	float pos[3];   // the marker's translation, in car space
	float dir[3];   // its third row: the way the lamp faces
	u8 type;        // CARKIT_LIGHT_HEAD or _BRAKE
	u8 side;        // CARKIT_LIGHT_LEFT, _RIGHT or _CENTRE
} CarKitLight;

// Stock cars carry four to six; the aftermarket lamps no more.
// Exhaust markers kept for the nitrous flame (two tips, or one, on retail cars).
#define CARKIT_EXHAUSTS 4
#define CARKIT_LIGHTS 16
#define CARKIT_DAMAGE_ITEMS 15   // cardamage.h CARDAMAGE_ITEMS
// CarRenderInfo's glass position lists (#513): 12..17 the windows, 20..23 the
// head- and brake-lights, in that order; a few markers a list.
#define CARKIT_GLASS_LISTS 10
#define CARKIT_GLASS_POS   2

// RenderTireMask's rim size without a front wheel part (Speed.elf
// 0x8033bfa8).
#define CARKIT_RIM_INCHES 17.0f

typedef struct {
	// Per paint slot: the save's paint part for it, or 0 when that slot falls
	// back to the body colour. Retail gives a component its own material only
	// when the component's part is aftermarket *and* its paint slot is filled,
	// and that gate is already applied here, so a caller just looks each
	// nonzero one up in PAINT.nfsp. [CARKIT_PAINT_BODY] is always 0.
	unsigned int paintPart[CARKIT_PAINT_SLOTS];
	int parts;      // part instances assembled, LOD A
	int verts;      // LOD A's
	int lodParts;   // and the LOD B, C and D instances beside them
	int lodVerts;
	int textures;

	// The underglow's tubes, off whichever front bumper, side skirt and rear
	// bumper the save actually fitted -- the markers move with the kit, so a
	// KIT02 bumper hangs its tubes where its own bodywork is.
	CarKitNeonSeg neon[CARKIT_NEON_SEGMENTS];
	int neonSegs;
	// The save's NEON slot (CarSlotId 0x9B): the colour part, looked up in
	// NEON.nfsn. 0 when the slot is empty or past numSlots.
	unsigned int neonPart;
	// The engine bay's and the trunk's tubes (TASKS.md #506): the CarRenderInfo
	// constructor's ENGINE_NEON_START_<n>/_END_<n> (n < 5) off the ENGINE
	// slot's model 0 and MARKER_TRUNK_NEON_START_<n>/_END_<n> (n < 4) off the
	// TRUNK_AUDIO layout's model 0, both in car space; and the save's
	// NEON_ENGINE (0x9C) and NEON_TRUNK (0x9E) colour parts. bayLit[0] / [1]:
	// the ENGINE / TRUNK_AUDIO part is level 3, which adds that neon's colour
	// to the bay's scaled light (model3d.h Model3DPartPose neonAdd).
	CarKitNeonSeg engineNeon[CARKIT_ENGINE_NEON];
	int engineNeonSegs;
	CarKitNeonSeg trunkNeon[CARKIT_TRUNK_NEON];
	int trunkNeonSegs;
	unsigned int engineNeonPart, trunkNeonPart;
	bool bayLit[2];

	// The light flares, in the order the slots carry them.
	CarKitLight light[CARKIT_LIGHTS];
	int lights;
	// The save's HEADLIGHT_BULB slot (CarSlotId 0xA0): the colour part the two
	// headlight flares take, looked up in CARLIGHTS.nfsl. 0 when empty.
	unsigned int bulbPart;
	// RenderFlaresOnCar (Speed.elf 0x80237158, SPEED2.EXE 0x60c8a0) keeps the
	// centre brake light dark on the CIVIC, TIBURON, SENTRA, 3000GT, GTO and
	// IMPREZAWRX once the SPOILER slot holds an aftermarket part.
	bool centreBrakeOff;

	// The render box UpdateCarParts (Speed.elf 0x802334d0) keeps at
	// CarRenderInfo+0x10/+0x20, in car space: the union of the models in the
	// BASE..ROOF slots and the five damage slots, 0.1 m out on every side.
	// carshadow.c lays the drop shadow over it.
	float shadowMin[3], shadowMax[3];

	// The four rims' placements, FL FR RR RL, in car space as the rims are
	// drawn (row vectors, p' = p * m; local +y runs inboard from the rim's
	// face), and the front wheel part's rim size in inches: RenderTireMask's
	// inputs, for carshadow_draw_tiremask(). rims is 0 for a kit with no
	// wheel table.
	float rim[4][16];
	int rims;
	float rimInches;

	// The Nitrous Purge's hood nozzles (TASKS.md #372): UpdateCarParts
	// (Speed.elf 0x802334D0) puts them on markers 0xE3DE69DB (left) and
	// 0x601A6CCE (right) of the BASE slot's model 0, at the marker's
	// translation. purgeHoodOk[i] is false when the model has no such marker.
	float purgeHood[2][3];
	bool purgeHoodOk[2];

	// The exhaust tips the nitrous flame comes out of (TASKS.md #317):
	// CarRenderInfo's list 10, the translations of the exhaust markers on
	// model 0 of the drawn parts (Speed.elf 0x802314C0), in car space.
	float exhaust[CARKIT_EXHAUSTS][3];
	int exhausts;

	// What CompositeSkin reads off the save for the car's skin, for
	// carskin_compose(); model3d_set_skin() puts the result on the model.
	CarSkinDesc skin;

	// The breakable items' markers (#324, cardamage.h's order): each one's
	// translation in car space, x and y, where damageItems has its bit.
	float damageItem[CARKIT_DAMAGE_ITEMS][2];
	// Where the broken glass bursts (#513): CarRenderInfo's position lists
	// 12..17 and 20..23 (CARKIT_GLASS_LISTS' order), each marker's translation
	// in car space, glassPosN of them a list.
	float glassPos[CARKIT_GLASS_LISTS][CARKIT_GLASS_POS][3];
	unsigned char glassPosN[CARKIT_GLASS_LISTS];
	unsigned int damageItems;
} CarKitInfo;

// Builds `car` (its CARS/ folder name, e.g. "TIBURON") out of `slots`, a
// career record's PresetCarSlot array indexed by CarSlotId (numSlots of them).
// `dataRoot` is the game data directory as a full path ("sdmc:/nfsu2data").
//
// Returns NULL when the car's own kit or PARTMODELS.nfsm is missing or
// malformed, or nothing could be assembled -- the caller then falls back to
// the stock .nfsc. A missing *library* kit only drops the parts that live in
// it, and says so through `log` (which may be NULL).
Model3D* carkit_assemble(const char* dataRoot, const char* car,
	const unsigned int* slots, int numSlots, CarKitInfo* info,
	void (*log)(const char* msg));

// Just the light fields of `info` (light, lights, bulbPart, centreBrakeOff)
// for `car` wearing `slots`, without building a model: what a car drawn from
// its stock .nfsc -- a race opponent -- needs for its flares. Everything else
// in `info` is zeroed. False when the car's kit or PARTMODELS.nfsm is missing.
// Whether the next assemblies carry the cracked panes (CARS/GLASS.nfsk,
// #324): on by default. A car whose damage can never show (a race
// opponent outside a drag) is built without them, saving 128 KB.
void carkit_glass(bool on);
// Whether the next assemblies carry the trunk audio install (TASKS.md #314):
// Render draws it only on the frontend car and the player's (its
// GetDriverInfo test), so a race opponent is built without it. On by
// default.
void carkit_audio(bool on);

bool carkit_lights(const char* dataRoot, const char* car,
	const unsigned int* slots, int numSlots, CarKitInfo* info,
	void (*log)(const char* msg));

// TASKS.md #402: a shop preview builds the whole car again for every part
// the cursor rests on. While the cache is held, PARTMODELS.nfsm, the opened
// kits and the vertex, index and texture runs read out of them stay in RAM
// from one build to the next (up to a few MB), so a rebuild reads nothing off
// the card. hold(false) frees all of it; a build made while it is not held
// keeps nothing, as before.
void carkit_cache_hold(bool hold);
bool carkit_cache_held(void);

// #348: held across a world load, PARTMODELS.nfsm and the opened kits' tables
// stay in RAM from one car build to the next (the player's car, then the
// racers'), so each build after the first reads neither again; no runs are
// kept. hold(false) lets go of them unless a shop hold still has them.
// carkit_cache_held() answers for the shop hold only.
void carkit_cache_hold_load(bool hold);
