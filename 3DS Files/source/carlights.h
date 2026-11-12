// Car light flares (TASKS.md #296): the glow over a car's head- and
// brake-lights.
//
// The lamps are not lights in the model. Each HEADLIGHT and BRAKELIGHT part
// carries LEFT_/RIGHT_/CENTRE_ markers (carkit.c collects them into
// CarKitLight), and RenderFlaresOnCar (Speed.elf 0x80237158) hands each one
// to eRenderLightFlare (0x800cbfcc): a camera-facing quad, pulled toward the
// eye so the body does not hide it, that grows and brightens as the lamp
// turns toward the camera and fades out between 179 and 200 m. A headlight
// draws two layers, HEADLIGHTFLAREINNER and HEADLIGHTFLAREOUTER, in the
// colour of the car's HEADLIGHT_BULB part; a brake light draws one,
// HEADLIGHTGLOW, in a fixed red that is dim at rest and full while braking.
//
// The headlights also light the road: RenderCarsTexturedHeadlights lays
// 2PLAYERHEADLIGHT1 flat ahead of every car whose lamps are on (see
// roadLight() in carlights.c).
//
// The textures and the bulb colours come from GLOBAL/CARLIGHTS.nfsl
// (tools/carlights_pack.py). Without it nothing is drawn.
//
// There are no lit lamp textures to draw: every light state retail picks is
// an _OFF one, and a lit lamp is its flares (TASKS.md #296, slice 2).
//
// The frontend's cars get the flares alone (RenderFEFlares, CARLIGHTS_FRONTEND).
//
// And the player's car throws its beam onto the world: the PC's projected
// headlight, HEADLIGHTS through a camera over the car's nose
// (carlights_headlight(), drawn by model3d.c's world batches).
//
// And the road's reflection draws every car's flares and light on the road a
// second time, into its capture (CARLIGHTS_REFLECTION).
//
// The world's lamps glow through the same flare (carlights_world_draw(),
// TASKS.md #323), and so do the reflectors and lamps on its props.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#include "carkit.h"
#include "carneon.h"
#include "lightdb.h"
#include "model3d.h"

typedef struct {
	CarKitLight light[CARKIT_LIGHTS];
	int lights;
	u8 bulb[3];          // the headlight colour, GC's halved channels
	u8 beam[3];          // the same at full strength, SPEED2.EXE's +0x380
	bool centreBrakeOff;
	bool set;            // set up from a kit; a cleared one draws nothing
} CarLights;

// How far ahead of the car's origin its foremost headlamp sits, in metres
// (0 for none), for the logs. (#384 anchored the road light to it; undone
// 2026-11-09, retail starts it 3 m ahead of every car.)
float carlights_head_x(const CarLights* l);

// What RenderFlaresOnCar reads off the Car each frame.
enum {
	CARLIGHTS_HEAD_ON = 1,   // Car+0x93c: UpdateHeadlightsAndHorn's lamps-on
	CARLIGHTS_BRAKING = 2,   // Car+0x944: UpdateBrakelightsState's brake lamp
	// RenderFEFlares (Speed.elf 0x80240550): the flares alone, with the
	// inner headlight row's power at RenderFlares' non-racing 2.8.
	// RenderCarsTexturedHeadlights, the light on the road, runs over the
	// world's cars only.
	CARLIGHTS_FRONTEND = 4,
	// Into the road's reflection (#296, reflection.h): eDisplayFrame's views
	// 4 and 5 run RenderFlares(view, 1) and RenderCarsTexturedHeadlights, so
	// the flares are flareType 1 (carlights.c's flare()) and the light on the
	// road is drawn as ever.
	CARLIGHTS_REFLECTION = 8,
	// g_LightGlowEnable 0 (#326): the light on the road alone. SPEED2.EXE's
	// car flares (FUN_0061AE90) test the glow value themselves, in every view;
	// RenderCarsTexturedHeadlights does not.
	CARLIGHTS_NO_FLARES = 16,
	// #394: no wheel of the car is down (CarSim onGround false, physics
	// running). The light on the road then fades from its drape onto the
	// road to retail's flat quad in the car's floor plane as the car rises.
	CARLIGHTS_AIRBORNE = 32,
	// The rear-view mirror (#501): SPEED2.EXE's view 3 runs the car flares
	// (FUN_0061ae90) and not RenderCarsTexturedHeadlights, so no light on the
	// road; for that view the inner headlight row's sizes are x2.7 and its
	// power x0.75 (0x7a6764 / 0x7a6760 on 0x7f3a30..48).
	CARLIGHTS_MIRROR = 64,
};

void carlights_set_log(void (*sink)(const char*));

// Loads CARLIGHTS.nfsl. False (and a log line) when it is missing or bad;
// every other call is then a no-op.
bool carlights_open(const char* path);
void carlights_close(void);

// The flares and bulb colour carkit_assemble()/carkit_lights() found.
void carlights_setup(CarLights* l, const CarKitInfo* info);
void carlights_clear(CarLights* l);

// Once a frame, before the first carlights_draw: starts this frame's half of
// the vertex buffer, so every car drawn this frame keeps its vertices until
// the GPU has read them.
void carlights_frame(void);

// One car's flares and its headlights' light on the road, in the world pass
// after its body. `rot`/`origin` are the transform the body is drawn with;
// `flags` is CARLIGHTS_*, `gain` as carneon_light_env() takes it.
void carlights_draw(const CarLights* l, const C3D_Mtx* vp, const CarNeonView* v,
	const float rot[3][3], const float origin[3], u32 flags, const float gain[3]);

// The car's own two lights in the light database (TASKS.md #166), as
// UpdateCarDynamicLights (Speed.elf 0x80241650) poses them each frame: a
// spot at the car looking ahead in the bulb's colour while the lamps are on
// (black when they are off), and one a metre behind it looking back in red.
// They light the other cars, never their own (CarRenderInfo::Render takes
// its pack out of the query first). `out` gets both.
#define CARLIGHTS_DYNAMIC 2
void carlights_dynamic(const CarLights* l, const float rot[3][3], const float origin[3],
	u32 flags, LightDbLight out[CARLIGHTS_DYNAMIC]);

// The world's light flares (TASKS.md #323): the glow on the street lamps,
// signs and windows, the same camera-facing quad as a car's, from the
// streamed sections' light flare packs (tools/flare_pack.py ->
// TRACKS/L4RA/WORLD.NFSF). Optional: false (and a log line) without the
// file, and the draw is then a no-op. The textures are CARLIGHTS.nfsl's, so
// nothing draws without carlights_open() either.
bool carlights_world_open(const char* path);
void carlights_world_close(void);

// The ground under a lamp, for its reflection: TopologyCoordinate::
// GetElevation at `pos`'s XY, nothing higher than `pos[2]`. False over no
// drivable section or no road.
typedef bool (*CarLightsGround)(const float pos[3], float* outZ, void* ud);
void carlights_world_set_ground(CarLightsGround fn, void* ud);

// The props' flares (the light flare pool): which model the player's view
// drew a marked scenery instance at this frame -- section `section`'s, at
// translation `pos` -- 1 its A, 2 its B, 0 neither, -1 there is none there
// (world.h's world_instance_lod()). `ref` is the instance's own, zeroed at
// open, for the callback to find it again by.
typedef struct {
	u32 serial;
	Model3D* model;
	u32 inst;
} CarLightsInstRef;
typedef int (*CarLightsInstLod)(int section, const float pos[3], CarLightsInstRef* ref,
	void* ud);
void carlights_world_set_instances(CarLightsInstLod fn, void* ud);

// A light flare pool entry that is no prop's (#497): the flag girl's flares,
// eGetNextLightFlareInPool(1, 3) with type 14 and flags 2, which
// carlights_world_draw()'s main view draws with the props' until the slot
// (0..3) is set again or cleared (`pos` NULL).
void carlights_pool_flare(int slot, int type, const float pos[3]);

// Each world step, `dt` seconds of world time: eResestLightFlarePool's blink,
// which turns the median poles' lamps on and off each half second.
void carlights_world_tick(float dt);

// The player's car, whose headlights light the reflectors the props carry:
// its position and unit forward vector, or NULL for no car.
void carlights_world_set_car(const float pos[3], const float fwd[3]);

// eRenderWorldLightFlares (Speed.elf 0x800cd1bc): the flares of each section
// in `sections` -- the visible list of the drivable section under the eye,
// the loaded ones (world.h's world_visible_loaded()) -- whose pack's box is
// in view. `flags` 0 is the main view's (flareType 0), after the cars'
// flares; CARLIGHTS_REFLECTION the road's reflection's (flareType 2): each
// lamp's mirror image in the road under it, stretched down the screen, into
// the capture after the cars'. Same state as carlights_draw().
//
// The main view's then draws the light flare pool (eRenderLightFlarePool,
// 0x800cbdb0): the flares the props the last world_draw() drew carry, from
// the same sections.
void carlights_world_draw(const C3D_Mtx* vp, const CarNeonView* v, const u16* sections,
	int n, u32 flags, const float gain[3]);

// The player's projected headlight (TASKS.md #296): view 9's camera over the
// car as eUpdateHeadlightCamera places it, the PC's projection and clip, and
// the bulb's colour, for model3d_set_headlight() around the world sections.
// `rot`/`origin` are the car's. False until carlights_open() has loaded the
// file, or for a car pointing straight down; the world then draws plain.
bool carlights_headlight(const CarLights* l, const float rot[3][3], const float origin[3],
	Model3DHeadlight* out);
