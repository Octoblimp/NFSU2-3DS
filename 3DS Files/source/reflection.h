// The road's reflection (TASKS.md #296): eDisplayFrame's views 4 and 5.
//
// Before the main view, the GameCube draws the scene once more into a
// capture: every reflective prop upside down about its own origin
// (world_draw_reflection), the player's car mirrored about its own z = 0
// plane (RenderCarReflections), and every car's flares and light on the road
// (CARLIGHTS_REFLECTION). The road's wet batches -- retail's sub-mesh flag
// 0x40 -- then add that capture at their own place on the screen
// (model3d_set_reflection, scenerf.v.pica), with the road texture's alpha as
// the wetness.
//
// This holds the capture: a 128x256 RGB565 render target in VRAM with its own
// depth, in the top screen's orientation, sampled projectively, and a second
// one of its size for level 3's blur. Created outside a frame and kept, as the
// env map is.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

typedef struct Reflection Reflection;

// NULL (and a log line) when there is no VRAM or the shader will not load;
// the road is then drawn dry.
Reflection* reflection_create(void (*log)(const char*));
void reflection_destroy(Reflection* r);

// Binds the capture and clears it to black with the far depth. Draw the
// views' content through the player's view-projection, then bind the screen
// again. Leaves depth testing as the world draws it.
void reflection_begin(Reflection* r);

// The capture, for model3d_set_reflection(). NULL for a NULL reflection.
const C3D_Tex* reflection_texture(const Reflection* r);

// g_RoadReflectionEnable 3 (#326): FUN_005C7E40, the capture as it stands
// blurred through SPEED2.EXE's FILTER effect -- along the screen's x into a
// second target, then along its y back -- 1/3 at +-1 texel and 1/6 at +-3,
// the centre left out. In-game, after the props and the car's body and
// before the lights (FUN_005CAAD0). Leaves the capture bound and depth
// testing as the world draws it. False, having drawn nothing, when the second
// target could not be made.
bool reflection_blur(Reflection* r);

// #328: how the road reads past the capture's edge. Off (the default), a
// black border: nothing reflected there. On, the edge texel, the way the
// GameCube's copy clamps. The screen probe's burst switches it.
void reflection_set_edge_clamp(Reflection* r, bool edge);

// #328: the capture as the GPU left it, for the screen probe. Waits for the
// GPU to go idle and copies the capture out of VRAM, then writes its mean,
// brightest, lit share and corner texels into `stats`, and with `bmpPath` the
// capture as a 256x128 BMP the way round the screen shots are. False when
// there is no capture or no linear memory for the copy.
bool reflection_probe(const Reflection* r, const char* bmpPath, char* stats, int statsSize);

// `vp` times the mirror about the car's own z = 0 plane (the road under its
// wheels): the matrix RenderCarReflections draws the car through, for
// model3d_draw_car_xform() with the car's own `rot`/`origin`, which keep its
// light as it is.
void reflection_car_vp(C3D_Mtx* out, const C3D_Mtx* vp, const float rot[3][3],
	const float origin[3]);
