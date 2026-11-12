// The frontend's top screen as SPEED2.EXE presents it (TASKS.md #312): the
// room and the car drawn into a target of their own, then the PC's screen
// filters over them -- the overbright glow a card of its tier 3 runs in the
// frontend -- and the box down to the panel, in passes over the whole
// screen. Without it the frontend draws straight onto the panel's target, as
// before #312. How big the target and the glow are is fepost_mode()'s
// (#319).
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

// The scene target, the filter targets and the shader, made the first time
// and kept: citro3d will not free a target inside a frame. The scene buffer
// is made for the scene drawn up to `maxSx` x `maxSy` times the panel's
// 240x400 (1 or 2 each): 256x400 at 1x1, 512x800 at 2x2. False, logged,
// without the VRAM or the shader; later calls give the same answer without
// trying again.
bool fepost_init(void (*log)(const char*), int maxSx, int maxSy);
// #352: the targets' VRAM given back while the world is open (the frontend
// never draws then; worldExit()'s callers all head back to it). Outside a
// frame. The next fepost_init() makes them again; the shader and the quad
// stay.
void fepost_release(void (*log)(const char*));

// The layout from the next fepost_begin() on: the scene `sx` x `sy` times the
// panel (1 or 2 each, at most what fepost_init() made room for) -- 2 along
// the panel's 240 doubles the screen's vertical resolution, which is what
// smooths a side-on car's sills -- and the glow the panel's size over
// `glowDiv` (1 or 2).
void fepost_mode(int sx, int sy, int glowDiv);

// Opens the frame's scene: with `clear`, clears the scene target -- colour
// black and depth 0, as C2D_TargetClear() leaves the panel's, with the memory
// fill -- then binds it and sets the viewport to the scene's part of it.
// Without, the caller puts the whole of both buffers back itself (fe_cache.h).
// Flush citro2d's batch first (sceneBegin()).
void fepost_begin(bool clear);

// The scene target, for fe_cache.c to copy; NULL before fepost_init().
C3D_RenderTarget* fepost_target(void);

// Binds the scene target again, with its viewport, after something inside the
// 3D has bound another (the reflection's faces).
void fepost_bind(void);

// With `filters`, after the car's body and before its neon and flares: the
// glow from the scene so far, then the scene target bound again. SPEED2.EXE
// draws those lights into the glow's own buffer, not the scene, and that
// buffer keeps part of itself from frame to frame; held still, they settle
// there at 1.89, 1.55 and 3 times what one frame adds (fe_post.c). That gain
// is what this returns, for carneon_draw() and carlights_draw(). NULL
// without the filters (or the glow's VRAM): the lights then go in as the
// world draws them.
const float* fepost_glow(bool filters);

// The screen filters (with `filters`; without, the scene alone) and the box
// down into `out`, the panel's own 240x400 target, which is left bound with
// its colour written everywhere and its depth 0 -- the clear the 2D after it
// expects. Runs the glow first when fepost_glow() has not. Restores
// citro3d's depth map and the 3D's depth test, and turns off the texture
// units the filters borrowed.
void fepost_end(C3D_RenderTarget* out, bool filters);
