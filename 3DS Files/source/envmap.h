// The in-world car's env map (TASKS.md #166): the GameCube's reflection,
// which is what puts the street on the paint and the chrome instead of black.
//
// Retail renders six 64x64 faces of the world around the player's car each
// frame -- three of them, on its own schedule -- from a camera 0.95 m over
// the car whose frame is turned to the world camera (UpdateEnvironmentMap-
// Cameras), and folds them into a 128x128 sphere map (cSphereMap::
// genSphereMap). Every car's env-mapped batches then add that map's texel at
// their normal (model3d.c, drawEnv). envmap.c is that capture and that fold;
// world.c's world_draw_env() draws a face's scenery.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#include "world.h"

typedef struct EnvMap EnvMap;

// The six face render targets and the sphere map, in VRAM. NULL, logged,
// without the memory or the shader. citro3d will not delete a render target
// inside a frame, so main.c makes one the first time the world opens and
// keeps it; envmap_destroy() is for outside a frame only.
EnvMap* envmap_create(void (*log)(const char*));
void envmap_destroy(EnvMap* e);

// Forgets the faces, for a world just entered: the next envmap_render()
// draws all six rather than keeping the last place's.
void envmap_reset(EnvMap* e);

// Retail's update schedules: 0 renders faces 0 and 4 every frame and one of
// 1, 2, 3 and 5 in turn (three a frame), 1 one face a frame round the six,
// 2 all six every frame. SPEED2.EXE picks one by g_CarEnvironmentMapEnable
// (FUN_005c3450's table at 0x7FEEA0): 1 is mode 1, 2 mode 0, 3 mode 2.
// PORT (#432): 3 is the 3DS's -- two faces a frame round three ({0,4}
// {2,5} {1,3}), what levels 2 and 3 run instead of mode 0.
void envmap_set_schedule(EnvMap* e, int mode);
// #349: called after each face is drawn and after the sphere's fold, so the GPU
// starts on them while the CPU records the rest (world_gpu_kick_clean()).
// NULL (the default) never kicks.
void envmap_set_kick(void (*kick)(void));
// Test only (DbgEnvFaceOff): faces in `mask` (bit k = face k) are cleared
// black once and never drawn, to weigh each face's cost against its look.
void envmap_set_face_skip(EnvMap* e, unsigned mask);

// One frame's capture, inside the frame and before the top screen's world:
// this frame's faces from `envEye` (the car's position + 0.95 m up), in the
// frame of a camera at `camEye` looking at it, then the sphere map. Leaves
// the GPU drawing on no target -- the caller binds its own again -- and hands
// model3d.c the sphere map and its axes for the cars (model3d_set_car_env).
void envmap_render(EnvMap* e, const World* w, const float camEye[3],
	const float envEye[3], float ambient, float gain, float invGamma);

// The same capture with something other than the world in the faces: `draw`
// draws one face's scenery through `vp` from `eye` looking along `fwd`, and
// returns the triangles it submitted. The frontend's room goes in this way
// (eDataRender's car takes TheOnlyEnvMap's sphere map whenever
// CARSELECTENVMAP is missing, as it is from every retail build's data).
// `turned` puts in each face what lies half a turn from it about the env
// camera's right axis -- up for down, ahead for behind -- which is how
// SPEED2.EXE's frontend car reads its live cube (envmap.c says why).
typedef u32 (*EnvMapDraw)(void* ctx, const C3D_Mtx* vp, const float eye[3],
	const float fwd[3]);
void envmap_render_with(EnvMap* e, EnvMapDraw draw, void* ctx, const float camEye[3],
	const float envEye[3], bool turned);

// The debug view (DbgEnvMap=2): the six faces and the sphere map, drawn with
// citro2d on whatever 2D scene is open, each the way up its texture says.
void envmap_draw_debug(const EnvMap* e);

// Triangles the last envmap_render() submitted for its faces, and how many
// faces it drew.
void envmap_stats(const EnvMap* e, u32* outTris, int* outFaces);
