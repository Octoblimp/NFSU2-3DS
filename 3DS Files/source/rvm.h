// The rear-view mirror (TASKS.md #501; tools/re/wip501/NOTES.md has the trace).
//
// SPEED2.EXE: with the Rearview Mirror option on, the Bumper and Hood
// cameras get HUD feature 0x1000 (FUN_00600090), and Player::
// SetRearViewMirrorCameraMover puts a RearViewMirrorCameraMover on eView 3:
// the car's own frame turned to look straight back, 0.5 m over the car
// (RVMOffsetInCar), 110 degrees square (bAngle 20000), near 0.75, far 192.
// FUN_005caad0 renders that view into its 256x256 target -- cleared black,
// the scenery under cull flags 0x1000, the other cars, their flares and the
// world's -- then blends RVM over it and writes RVM_MASK's alpha. FUN_005cbf40
// draws the target, mirrored left to right, at x 200..440, y 15..95 of the
// 640x480 screen, under the HUD.
//
// Here: a 256x64 target (the 256x256 squashed into the same 3:1 the screen
// shows it at), the env faces' scenery draw (world_draw_env: the 0x1000 set,
// see world.h), and one quad that does the RVM blend and the mask's cut in
// a single TEV pass at x 125..275, y 7.5..47.5 of the top screen.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#include "carneon.h"

typedef struct Rvm Rvm;

// The target (VRAM) and the quads (linear memory). NULL, logged, without
// them. Made once, the first time the world opens, and kept: citro3d will not
// delete a render target inside a frame.
Rvm* rvm_create(void (*log)(const char*));
void rvm_destroy(Rvm* r);

// Starts the mirror's view, inside the frame and before the top screen's: the
// target bound and cleared black, `vp` the view-projection from `eye` looking
// along `fwd` with `up`, `view` the same camera for carlights_draw(). The
// caller draws the scenery and the cars, then binds its own target again.
void rvm_begin(Rvm* r, const float eye[3], const float fwd[3], const float up[3],
	C3D_Mtx* vp, CarNeonView* view);

// The mirror on the bound top-screen target (an eye of it): the view mirrored
// left to right, darkened by `frame`'s alpha and cut to `mask`'s. Either
// NULL: the plain rectangle. Leaves depth testing and blending as the world
// draws them.
void rvm_draw(Rvm* r, const C3D_Tex* frame, const C3D_Tex* mask);
