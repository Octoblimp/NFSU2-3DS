// Light Trails (TASKS.md #326): SPEED2.EXE's g_LightStreaksEnable
// (DAT_00870ce0, the display block's +0x30), with Light Glow on, and the
// Over Bright and Depth of Field passes that share its buffers. See trails.c.
//
// Once a frame, trails_frame(); then, for each eye, trails_begin() before the
// flare pass (every carlights_draw() and carlights_world_draw() of the main
// view), trails_end() after it, and trails_post() at the view's end.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

void trails_set_log(void (*sink)(const char* msg));

// `on` (Light Trails and Light Glow both set, the world drawn), `overBright`
// (Over Bright set too: the composite waits for trails_post()), `dt` seconds
// since the last frame. Call before any trails_begin() this frame.
void trails_frame(bool on, bool overBright, float dt);

// Before the flare pass of eye `eye` (0 the top screen's, 1 the right eye),
// with `eyeTarget` bound and its 3-D drawn: fades the eye's trail buffer and
// binds it in place of `eyeTarget`, on `eyeTarget`'s depth. False when the
// trails are off or have no buffer: the flares then draw on `eyeTarget` as
// they always did, and trails_end() must not be called.
bool trails_begin(C3D_RenderTarget* eyeTarget, int eye);

// After the flare pass that trails_begin() took: adds the buffer over
// `eyeTarget` (with Over Bright, trails_post() does) and binds it again. Leaves the depth test and blend as
// smear_draw() does.
void trails_end(C3D_RenderTarget* eyeTarget, int eye);

// With Over Bright, at the end of eye `eye`'s view (the PC's visual
// treatment, before the horizon fog): the frame's glow into the buffer, then
// the buffer over `eyeTarget`, left bound. False, and nothing drawn, when
// Over Bright is off or the eye's buffer was not drawn this frame.
bool trails_post(C3D_RenderTarget* eyeTarget, int eye);

// Depth of Field (#326), at the end of the eye's view before the Bleach
// Bypass grade, with `eyeTarget` bound and its depth buffer still the
// frame's: blurs the frame where it lies off the camera's sharp span --
// `focus` its middle and `span` its length, metres (nis_dof()) -- as the
// PC's DOF pass does. `depthAt` turns a view depth into the depth buffer's
// value (the world's projection through C3D_DepthMap(true, -1, 0)); `nearZ`
// and `farZ` are that projection's planes. Leaves `eyeTarget` bound, the
// depth test and blend as the world's passes expect them. False when nothing
// was drawn.
bool trails_dof(C3D_RenderTarget* eyeTarget, float focus, float span, float nearZ,
	float farZ, float (*depthAt)(float z));

// The buffers' memory back, outside a frame (the world closing).
void trails_release(void);

// The left eye's buffer (colour and its own depth, both VRAM) made at the
// world's opening, ahead of the bank's textures, when Light Trails will run:
// `width` the eye target's (240).
void trails_world_buffer(bool want, u16 width, GPU_DEPTHBUF depthFmt);
