// Stereoscopic 3D on the top screen (TASKS.md #326).
//
// PORT: the PC has no stereo; the user asked for it, with settings for how
// it looks (display.h's stereo fields, kept in the profile's .display file).
// The street is drawn once per eye, through Mtx_PerspStereoTilt: the left
// eye into the top screen's own target, the right into one of this module's
// that shares its depth buffer. The 2D over the street -- the HUD, the Engage
// tab, the pause menu, the EA Trax popup -- is drawn once, into an overlay of
// its own, and that is laid over both eyes a few pixels apart, so it stands
// in front of the street. Only the world is in stereo; the frontend's car and
// room stay flat (the panel shows the left image to both eyes).
//
// How it looks, from the 3D slider and display.h's steps:
//   depth  how far apart the eyes' images of the far distance are, up to
//          STEREO_PX_PER_DEPTH pixels a step at the slider's top
//   focus  how far ahead of the camera the screen's plane sits: nearer than
//          that stands out of the screen, further in
//   HUD    how far the 2D stands out of the screen, STEREO_PX_PER_HUD a step
//          shared between the eyes
// The eyes' separation is worked out from the depth in pixels, so the far
// distance stays as deep whatever the chase camera's field of view.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#define STEREO_PX_PER_DEPTH 1.2f   // pixels of the far distance's separation a depth step
#define STEREO_FOCUS_MIN    1.2f   // the screen's plane at focus 0, in metres
#define STEREO_FOCUS_STEP   0.8f   // and further a focus step: 5.2 m at the
                                   // default 5, about the chase camera's car,
                                   // which then sits in the screen's plane
#define STEREO_PX_PER_HUD   1.0f   // the 2D's separation a HUD step
// 3-D Style Pop Out (#365): the screen's plane this far further ahead, and
// the far distance's separation cut by the same ratio, so the picture's whole
// range of depth is Into Screen's, moved forward: the car and the road near it
// stand out of the glass and only the far distance goes in. The HUD stays in
// front of the car by the car's own pop.
#define STEREO_POP_EXTRA    15.0f

// The right eye's target -- 240x400 like `left`, sharing its depth buffer,
// shown on the top screen's right side with `transferFlags` -- the overlay
// and the shader the overlay is laid on with. Made once, outside a frame,
// and kept. False, logged, without the VRAM, linear memory or the shader:
// the top screen then stays flat.
bool stereo_init(void (*log)(const char*), C3D_RenderTarget* left, u32 transferFlags);

// Right after the frame's clears: nothing of last frame's stereo carries over.
// Also turns the panel's 3D mode on or off as display.h's setting asks.
void stereo_frame_start(void);

// Whether this frame's world goes down twice: the setting on, the 3D slider
// up and the targets made. `allowed` false keeps it flat whatever those say.
// Works out this frame's separations.
bool stereo_world_begin(bool allowed);

// Eye `eye`'s projection (0 the left, 1 the right) for a view whose
// horizontal half-angle has tangent `tanHalfX`: the eye's offset for
// model3d_camera_up_stereo(), signed, and the screen plane's distance.
void stereo_eye(int eye, float tanHalfX, float* iod, float* screen);

// After the left eye's 3D and its full-screen passes, whether the right eye
// still fits in the GPU's command buffer: what the left eye wrote again, a
// quarter more, and STEREO_CMD_RESERVE words for the 2D, the bottom screen
// and the frame's end. A whole frame's commands go into the one buffer
// (C3D_Init's), and the right eye writes about as many as the left; overrun,
// libctru panics. False turns the rest of the frame flat, logged: the right
// target is not drawn on, so the panel shows the left image to both eyes.
#define STEREO_CMD_RESERVE 0x8000
bool stereo_right_fits(void);

// The right eye's start, after stereo_right_fits():
// its target cleared to `clearColour` (C2D_Color32) with the shared depth at
// its far value, the overlay cleared and the right eye's target bound.
void stereo_right_begin(u32 clearColour);

// The right eye's target, for C2D_SceneBegin(); NULL before stereo_init().
C3D_RenderTarget* stereo_right(void);

// From here to stereo_composite(), the top screen's 2D goes into the overlay:
// sceneBegin() hands its target to stereo_scene_begin() first.
void stereo_overlay_begin(void);

// True when `target` is the left eye's and the overlay is open: it is then
// bound for citro2d in the top screen's place. False otherwise, with nothing
// done.
bool stereo_scene_begin(C3D_RenderTarget* target);

// The overlay over both eyes, the HUD's separation apart, and the overlay
// closed. Nothing when it was not opened this frame. Leaves citro2d ready to
// draw (C2D_Prepare) with C3D_Init's blend and depth state.
void stereo_composite(void);

// The config's DbgStereo and DbgStereoSwap (main.c): the slider as if at
// `slider` when it is above 0, and with `swap` each eye on the other side.
// With the slider set here, the log gets a "stereo check" now and then: the
// eyes' separations measured from the framebuffers, against the set ones.
void stereo_debug(float slider, bool swap);

// One line for the debug log: the setting, the slider and the separations.
void stereo_describe(char* out, int outSize);

// #328: the top screen as the display controller scans it out, for the debug
// log -- the GSP's framebuffer format word (bit 5 set: the right eye's
// columns come from the right framebuffer), the two buffers it shows and the
// mean green of a sampled grid of each, beside the slider and whether the
// right eye was drawn. `left` and `right`, when given, get the shown buffers
// (NULL where an address is not one the CPU can read).
void stereo_present_probe(char* out, int outSize, const u8** left, const u8** right);

// #328: the mean green of the shown left buffer alone, cheap enough to take
// every few frames; `left`, when given, gets that buffer (or NULL).
unsigned stereo_shown_green(const u8** left);
