// A car's drop shadow and neon ground glow (TASKS.md #257), transcribed from
// the GameCube build's CarRenderInfo::Render (Speed.elf 0x80239018), which
// has the symbols.
//
// Retail lays a 4x4 grid of vertices over the road under the car's render box
// (CarRenderInfo+0x10/+0x20, carkit.c's shadowMin/Max), drops each vertex onto
// the road with GetElevation, and draws the three strips between its rows with
// <CAR>_SHADOWIG. When the car has neon, it draws the same strips again with
// <CAR>_NEON in the underglow's colour: that is the pool of light under it.
//
//   0x80239c74  the ground point under the car, and its normal
//   0x80239ce0  the height factor: how far the car is off the road
//   0x8023a710  the grid's box, scaled up as the car rises
//   0x8023a960  each wheel's elevation and slope, for vertices off the road
//   0x8023aa78  each vertex's elevation, and which strips to drop
//   0x8023ac54  the shadow's alpha
//   0x8023b0c0  the glow's colour (+0x354)
//   0x8023b42c  the strips
//
// The textures come from CARS/<CAR>.nfss (tools/carshadow_pack.py). Traffic
// cars ship none, and retail then draws no grid for them either.
//
// At g_CarShadowEnable 2 retail draws a second, softer shadow in the world
// before the grid: the car's collision hull cast along the light onto the
// road, on SHADOWRAMP (0x80239de4 .. 0x8023a710): carshadow_draw_hull().
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include <tex3ds.h>
#include "carsim.h"

enum {
	CARSHADOW_TEX_IG = 0,   // <CAR>_SHADOWIG, the world's shadow
	CARSHADOW_TEX_FE,       // <CAR>_SHADOWFE, the frontend's
	CARSHADOW_TEX_NEON,     // <CAR>_NEON, the glow
	CARSHADOW_TEX_COUNT
};

typedef struct {
	C3D_Tex tex[CARSHADOW_TEX_COUNT];
	bool    have[CARSHADOW_TEX_COUNT];
	bool    additive[CARSHADOW_TEX_COUNT];   // alphaBlendType 2
	float   boxMin[3], boxMax[3];   // the render box, in car space
	bool    ready;
} CarShadow;

void carshadow_set_log(void (*sink)(const char* msg));

// Reads CARS/<CAR>.nfss into `s`. False when it is absent or malformed -- a
// card packed before #257 has none -- and `s` is then left empty, so drawing
// it draws nothing and the caller keeps whatever it drew before.
bool carshadow_load(CarShadow* s, const char* path);
void carshadow_free(CarShadow* s);

// The car's render box, in the frame the body is drawn in: carkit.c's
// shadowMin/Max for an assembled car.
void carshadow_set_box(CarShadow* s, const float boxMin[3], const float boxMax[3]);

// The shadow, then the glow if `neon` (the pulsed underglow colour, packed
// R | G << 8 | B << 16 | A << 24 as carneon_colour() gives it) has alpha.
// The car is placed by `sim`'s rot and origin, as model3d_draw_car_xform
// takes them, and its wheels are the sim's WheelInfo positions; the road
// comes from `w`'s ground probe. Depth tested and not written. Puts blend,
// depth and the TEV scale back the way model3d.c leaves them.
void carshadow_draw(const CarShadow* s, const C3D_Mtx* vp, const CarSimWorld* w,
	const CarSim* sim, u32 neon);

// The hull shadow (CarRenderInfo::Render 0x80239de4, SPEED2.EXE 0x62193B):
// every point of the car's polytope (carsim_body_point) cast along the light
// from 2300 m over the world's origin, the convex hull round them dropped
// onto the road, its corners rounded, and the lot drawn as a fan from the
// middle with a rim round it fading out along SHADOWRAMP. `ramp` is
// SHADOWRAMP out of GLOBALB.BUN's Global\GlobalTextures.tpk (0xBADB4475),
// `sub` its decoded image. `lod` is the car's level of detail, 0..2 (retail
// draws none at D), and `lodFade` how far it is into C, which fades the
// shadow at C (world_car_lod_c_fade). Retail also gates it on
// g_CarShadowEnable 2, a world car (not the frontend's) and the view
// (FUN_0060D4D0 turns type 3 away); those are the caller's, which draws it in
// the main view only, as it does the grid. Depth tested and not written; draw
// it before carshadow_draw(). Puts the state back as carshadow_draw() does.
void carshadow_draw_hull(const C3D_Tex* ramp, const Tex3DS_SubTexture* sub,
	const C3D_Mtx* vp, const CarSimWorld* w, const CarSim* sim, int lod, float lodFade);

// The same grid in the frontend: the car is standing on a turntable, so there
// is no road to drop the vertices onto, nothing to lift them off and nothing
// to fade them -- retail flattens the lot onto one plane at a constant alpha
// (CarRenderInfo::Render's other side of cr2, 0x8023a710 / 0x8023ac10 /
// 0x8023ac88) and draws it with <CAR>_SHADOWFE. `pos` is where the wheels
// touch, model3d_draw_car's own, and `yaw` the car's angle about +Z. The
// neon glow goes over it from the same strips, in `neon` as for
// carshadow_draw(), now that the frontend draws the tubes casting it.
void carshadow_draw_fe(const CarShadow* s, const C3D_Mtx* vp, const float pos[3],
	float yaw, u32 neon);

// The frontend's tire masks (RenderTireMask, Speed.elf 0x80236918, SPEED2.EXE
// 0x620600, called from CarRenderInfo::Render only in the frontend): a black
// disc in each rim's plane, behind the spokes and the brake, so that a wheel
// seen from its inner side shows no daylight through it. `tex` is TIREMASK
// out of FrontB.lzc's Global\FrontEndTextures.tpk, `sub` its decoded image;
// `rim`, `rims` and `inches` are carkit.c's CarKitInfo rim, rims and
// rimInches; the car is placed by `rot` and `origin` as carneon_draw() takes
// them, and `fwd` is the camera's direction. Alpha tested, depth written, and
// the state put back as carshadow_draw() leaves it. Draw it after the body.
// PORT: a NULL `fwd` draws every rim's disc, TireFace or not, for
// DbgNoTireMask=3 to show where they sit.
void carshadow_draw_tiremask(const C3D_Tex* tex, const Tex3DS_SubTexture* sub,
	const C3D_Mtx* vp, const float (*rim)[16], int rims, float inches,
	const float rot[3][3], const float origin[3], const float fwd[3]);

// The NIS people's ground shadows (CBasicCharacterAnimEntity::RenderEffects,
// #497; crowd_shadow_quads() gives the quads): `n` ePolys, UVs (0,0) (1,0)
// (1,1) (0,1), vertex colour 0xFF808080 (SPEED2.EXE 0x433220), on `tex`
// (CHARACTERSHADOW: alpha-blended, no depth write). Puts the state back as
// carshadow_draw() does.
void carshadow_draw_quads(const C3D_Tex* tex, const Tex3DS_SubTexture* sub,
	const C3D_Mtx* vp, const float (*quad)[4][3], int n);
