// Car neon (TASKS.md #257): the underglow tubes, transcribed from the GameCube
// build's CarRenderInfo, which has the symbols.
//
//   UpdateCarParts     Speed.elf 0x80233fcc  which markers make the tubes
//                                            (carkit.c collect_neon())
//   UpdateNeonColours  0x80233348            the part's two RGBA colours
//   Render             0x8023ad6c            the per-frame pulse
//   RenderNeon         0x802377f0            which tube sets draw
//   RenderNeon         0x80237af4            one set: distance fade, the
//                                            screen-facing capsule per tube
//
// RenderNeon draws four sets -- the underglow, then the engine bay, cabin and
// trunk tubes (TASKS #506) -- but outside the frontend only the underglow
// shows, and that is retail's own doing, not a cut:
//
//   * The engine and trunk sets are scaled by their hood/trunk animation's
//     time, which is zero with the panel shut. Nothing in the world opens
//     either; the neon and trunk audio menus do (#314, #372 5c), and the
//     photo shoot's Show Off.
//   * The cabin set is handed the packed colour at CarRenderInfo+0x370, and no
//     code ever writes that word after the constructor zeroes it: Render
//     pulses the underglow (+0x358), engine (+0x364) and trunk (+0x37c)
//     colours and skips the cabin's. A zero alpha draws nothing, so on the
//     GameCube the cabin neon never lights whatever the save says.
//
// The ground glow under the car is a second, separate effect: Render redraws
// its drop-shadow strips with <CAR>_NEON in the colour at +0x354, which is
// carshadow.c's, lit with carneon_colour().
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include "carkit.h"

// The camera the tubes face, as RenderNeon reads it off the eView: the
// position (+0x40 of its camera), the camera matrix's first two columns, and
// the field of view (+0xC4) its distance fade is measured against. pxX/pxY are
// top-screen pixels per unit of view-space x/y at unit depth, which is what
// GetScreenPosition's direction is measured in.
typedef struct {
	float eye[3];
	float right[3];
	float up[3];
	float fwd[3];
	float pxX, pxY;
	u16   fov;        // bAngle units, 65536 to the circle
} CarNeonView;

// One car's neon: carkit.c's tubes and the save's NEON, NEON_ENGINE and
// NEON_TRUNK parts.
typedef struct {
	CarKitNeonSeg seg[CARKIT_NEON_SEGMENTS];
	int  segs;
	u32  part;
	CarKitNeonSeg engineSeg[CARKIT_ENGINE_NEON];
	int  engineSegs;
	u32  enginePart;
	CarKitNeonSeg trunkSeg[CARKIT_TRUNK_NEON];
	int  trunkSegs;
	u32  trunkPart;
	bool bayLit[2];   // CarKitInfo's: the ENGINE / TRUNK_AUDIO part is level 3
	double clock;     // WorldTimer, in seconds: it stops when the world does
} CarNeon;

void carneon_set_log(void (*sink)(const char* msg));

// GLOBAL/NEON.nfsn (tools/neon_pack.py): the colour table and NEONBLUR. False
// when it is absent or malformed -- a card packed before #257 has none -- and
// the cars then draw without neon, which the log says.
bool carneon_open(const char* path);
void carneon_close(void);

void carneon_setup(CarNeon* n, const CarKitInfo* info);
void carneon_clear(CarNeon* n);
void carneon_tick(CarNeon* n, float dt);

// The underglow's pulsed colour this frame, Render's +0x358, packed
// R | G << 8 | B << 16 | A << 24. 0 when the car has no NEON part or
// NEON.nfsn is not loaded. It does not need the tubes: the glow on the road
// is drawn from the part alone.
u32 carneon_colour(const CarNeon* n);

// Render's level-3 bay light (#506): out[0] the engine neon's pulsed colour
// (+0x364) when the ENGINE part is level 3, out[1] the trunk neon's (+0x37c)
// when the TRUNK_AUDIO part is, each r, g, b x (1/256) x (alpha/256); zero
// otherwise. For Model3DPartPose neonAdd.
void carneon_bay_light(const CarNeon* n, float out[2][3]);

// The neon of the car whose body is drawn with `rot`/`origin` (as
// model3d_draw_car_xform takes them), into the current target. Additive, depth
// tested and not written, unfogged, with `gain` as carneon_light_env() takes
// it. `bayT` is the hood's and the trunk's GetAnimationTime (Model3DPartPose
// animT[0] / [1]), which the engine and trunk tubes are faded by; NULL has
// both shut, the underglow alone. Puts blend, depth and the TEV back the way
// model3d.c leaves them.
void carneon_draw(const CarNeon* n, const C3D_Mtx* vp, const CarNeonView* v,
	const float rot[3][3], const float origin[3], const float gain[3],
	const float bayT[2]);

// The TEV and the blend of an additive light quad, the neon's and
// carlights.c's flares': texture times vertex colour, the colour doubled (the
// PC's world pixel shaders double colour, not alpha; afx.c says why), added in
// proportion to its alpha. `gain`, per channel and up to 4, brightens what it
// adds, as fe_post.c's light streaks have it (fepost_glow()); NULL adds it as
// the world does. carneon_light_env_end() puts the blend and the TEV back.
void carneon_light_env(const float gain[3]);
void carneon_light_env_end(void);
