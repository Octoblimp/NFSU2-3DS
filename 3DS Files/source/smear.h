// The speed smear and its nitrous boost: Speed.elf's eMotionBlurEffect
// (0x801DC9B4), the in-world screen effect g_MotionBlurEnable switches on
// (DrawSmear 2 on the GameCube). See smear.c.
//
// Once a frame, smear_update() with the camera and whether the player's
// nitrous look is on (UpdateAllScreenEmotion's palette effect 0); then, for
// each eye, smear_draw() over the finished 3-D, before the 2-D.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

void smear_set_log(void (*sink)(const char* msg));

// The camera's eye and its basis (right, up, forward, unit, world space) this
// frame, `dt` seconds after the last; `nos` the nitrous look (the player's
// bottle burning, not a blow-off, the car moving). Call once a frame, before
// any smear_draw(); `on` false (Motion Blur off, a cut) forgets the camera
// and lets the nitrous ramp go.
void smear_update(bool on, float dt, const float eye[3], const float right[3],
	const float up[3], const float fwd[3], bool nos);

// Over `target` (a 240x400 RGBA8 eye: the top screen's or the right eye's),
// bound with the 3-D finished in it: copies it, then draws the smears over
// it. False when nothing was drawn (too slow, off, or no buffer). Leaves
// `target` bound and the 3-D's depth test and alpha blend as the world's
// passes expect them.
bool smear_draw(C3D_RenderTarget* target);

// The eye's frame as a texture, for the rain's lens drops (#406): the
// capture smear_draw() takes, made here when `copied` is false (smear_draw()
// drew nothing this eye). 256x512 RGBA8, the eye's 240 columns and 400 rows
// at its top, as smear.c maps it; `target` is left bound. NULL without a
// buffer.
const C3D_Tex* smear_capture(C3D_RenderTarget* target, bool copied);

// The capture's texture, made ready without copying anything into it (the
// light trails, trails.c, copy their buffer in with smear_capture()). NULL
// without a buffer.
const C3D_Tex* smear_texture(void);

// For the log: the last frame's speed factor (0..1), the nitrous ramp
// (0..NOStimer) and its state (0 off, 1 rising, 2 falling, 3 full).
void smear_debug(float* speedK, float* nosT, int* nosState);

// #352: with fe_post's scene buffer released for the world, the capture takes
// 512 KB of VRAM of its own (`on`, at world entry, ahead of the bank's
// textures), given back at world exit (`on` false). Linear memory as before
// when VRAM has no room.
void smear_world_buffer(bool on);
