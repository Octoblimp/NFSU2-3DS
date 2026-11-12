// The weather (TASKS.md #315): retail's Rain class, one per player view, as
// the GameCube build has it (Speed.elf; decompiles in tools/re/wip315).
//
//   IsRainingAt              0x80251144  the rain front: |0.5 cos(x / 50 m +
//                                        WorldTimeSeconds, in degrees) + 0.5|
//   GetDesiredRainIntensity  0x80251098  the Rain layer's set under the car:
//                                        its intensity (field 0) where the
//                                        front is at or under its threshold
//                                        (field 1), else none
//   Rain::UpdateAndRender    0x802532B0  the intensity eased toward that over
//                                        WEATHERchange (1 s), the road's
//                                        dampness after it, then Update,
//                                        the haze screen effect and Render
//   Rain::Update             0x802514EC  400 drops in a box ahead of the car,
//                                        in the car's frame: the camera's
//                                        motion, the wind and a per-drop fall
//                                        each tick; killed under the tunnel
//                                        and overpass zones
//   Rain::Wind               0x8025237C  the Wind layer's prevailing
//                                        direction plus random gusts
//   Rain::Render             0x80252818  each falling drop a streak of
//                                        RAINDROP from its last position to
//                                        this one, brighter in the headlights
//   SetRainBase              0x8025092C  the World's constructor: the road
//                                        wet (dampness 1), every drop idle
//
// The dampness is what fades the road's reflection (#296): eDisplayFrame sets
// KColorWorldReflection's alpha to dampness * 255 for the reflection views.
// The haze is ScreenEffect type 2: the fog's colour (CalculateFogInfo writes
// it, the Night Raining Fog layer's mixed in by the intensity). The GameCube
// draws it as eScreenQuadReplace(0x6D), source over at alpha intensity * 128,
// a grey veil over half the frame in a full rain. PC (SPEED2.EXE
// FUN_005C7860, the user's reference) draws it as a 2x modulate instead
// (D3DBLEND_DESTCOLOR / SRCCOLOR, colour writes only) whose colour is
// ((c / 128 - 1) * I + 1) * 127.5: the Night Raining Fog grey (128) changes
// nothing, the rain only tints the frame toward the fog colour. The port
// draws PC's (the user, 2026-11-06, on the GameCube veil: "WAY TOO FOGGY").
//
// PORT: retail steps every per-tick term once a frame at a floating frame
// rate made for 60; here each one is scaled by dt * 60 so the rain falls as
// fast at the 3DS's rate. Split screen is not ported, so eGetCurrentViewMode
// is never above 2 and its 1/8 intensity never applies. OnScreenRain (the
// lens drops; PC's "Rain Splatter" option, rain_drop.efx over the captured
// frame) is PC's (#406, rain_lens_draw): the GameCube keeps their state and
// never draws them.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include "parammaps.h"
#include "trackpath.h"

void rain_set_log(void (*sink)(const char* msg));

// World entry: RAINDROP out of `globalBunPath` (GLOBAL/GLOBALB.BUN), the
// Rain, Wind and Night Raining Fog layers out of `pm` and the cover zones out
// of `paths` (each may be NULL: no rain, no wind, grey haze, no cover), then
// SetRainBase. False only when the texture would not load -- the rain is then
// still simulated, so the road dries and wets, but never drawn.
bool rain_open(const char* globalBunPath, const ParamMaps* pm, const TrackPaths* paths);
void rain_close(void);

typedef struct {
	float carPos[3];
	float carRot[3][3];   // rows: forward, left, up (world)
	float camEye[3];
	float camFwd[3];
	float worldTime;      // WorldTimeSeconds
	bool  playerDriven;   // the drop streaks stretch with the camera's motion
} RainInput;

// UpdateAndRender's update half, once a world frame while the game runs (not
// paused). `forceIntensity` >= 0 replaces GetDesiredRainIntensity (DbgRain).
void rain_update(float dt, const RainInput* in, float forceIntensity);
// Render: the streaks, in world space through `vp`. Call after the effects,
// before the 2D; it leaves the blend and depth state as afx_draw does.
void rain_draw(const C3D_Mtx* vp);

// OnScreenRain (#406), PC's "Rain Splatter" (g_RainEnable): up to 20 drops
// on the lens while it rains and the car is not under cover, each showing the
// frame around it through RAINDROPPC's refraction (rain_drop.efx). Active
// when one would be drawn. Draw with the eye's finished frame (smear.h's
// smear_capture) after the speed smear, before the 2D; `eye` 0 or 1 (the
// stereo right eye). Leaves the depth test and blend as smear_draw() does.
bool rain_lens_active(void);
void rain_lens_draw(const C3D_Tex* frame, int eye);
// DbgLensHold: the drops stop ageing and moving (retail's OverRide).
void rain_lens_hold(bool on);
bool rain_lens_held(void);
// One line for the log: each live drop's screen position, size and alpha.
void rain_lens_describe(char* out, int size);

float rain_intensity(void);    // Rain +0x1F0
float rain_dampness(void);     // Rain +0x43E8, 0..1
// The haze quad's colour, for a 2x modulate over the frame (frame * 2 * rgb
// / 255; 128 is about "no change"); false when there is none. `fogRgb` is
// the fog colour the frame is drawn with (0xRRGGBB).
bool rain_haze(u32 fogRgb, u8 rgb[3]);

// The world fog in the rain (#407). PC's CalculateFogInfo (SPEED2.EXE
// 0x614300) blends the Normal Fog set at the car toward the Night Raining
// Fog's by the intensity, and eDisplayFrame (0x5CB02D) hands the world's
// vertex shaders FogValue = { 250, 1 / (250 - start), floor }, floor = 1 -
// the density, at most 0.6. The shader (vs_1_1 in the world effects):
// oFog = min(max((250 - z) / (250 - start), floor), 1), z the eye depth, 1 =
// clear. `wet` is that with the rain blended in, `dry` without; false when it
// does not rain (they would be the same) or there is no Normal Fog layer.
// PORT: the port's world fog is its own (the draw-radius curve, #148, and the
// GameCube's, whose world never reads these layers); the rain's share of PC's
// fog, wet over dry, is laid on top of it (main.c rainFogTick).
#define RAIN_FOG_END 250.0f   // 0x7A27A0
typedef struct {
	float start;   // FogInfo[3], field 1
	float floor;   // FogValue.z
} RainFog;
bool rain_fog(RainFog* wet, RainFog* dry);
float rain_fog_visibility(const RainFog* f, float z);

// One line of state for the log.
void rain_describe(char* out, int size);
