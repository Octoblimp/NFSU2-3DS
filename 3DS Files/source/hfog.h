// Horizon fog (TASKS.md #326 item 4): SPEED2.EXE's glow of distant city
// lights over the skyline, g_HorizonFogEnable (0x8707CF). Not fog at all: a
// 10x10 grid over the whole screen, added on top of the finished street,
// whose vertex colours are the nearby-on-screen distant lights.
//
//   FUN_005C7470  the pass: the grid in clip space, rows nearer and more
//                 opaque toward the top, additive SRCALPHA/ONE, z tested and
//                 not written, after the main view of the player's eView
//   FUN_005DB900  per frame: each light's fade with distance and its screen
//                 position, the list sorted nearest first, the area's
//                 "Horizon Fog Visibility" as the intensity
//   FUN_005DBB00  one grid vertex's colour from the lights near it
//
// The lights are chunk 0x34250 of TRACKS/L4RA.BUN (loader 0x5E0190), type 6
// of its list. The GameCube's eInitHorizonFogDisplayList builds a different
// grid but nothing ever draws it, so the PC's is the one ported.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

// The camera the grid is laid over: the centre view's basis and the tangents
// of its half-angles (main.c's g_afxView and g_worldView).
typedef struct {
	float eye[3], fwd[3], right[3], up[3];
	float tanX, tanY;
} HfogView;

// Chunk 0x34250 out of `bunPath` (the track's L4RA.BUN). False when it has
// none or it is malformed, and the pass then draws nothing.
bool hfog_open(const char* bunPath, void (*log)(const char* msg));
void hfog_close(void);
bool hfog_loaded(void);

// Once a frame, before the eyes: the lights' fades and screen places, and
// the grid's colours and world positions. `intensity` is the area's Horizon
// Fog Visibility (1 where there is no table, as on the PC).
void hfog_update(const HfogView* v, float intensity);

// The last update's lights in front of the camera, and the most any grid
// vertex adds per channel (colour times its row's alpha), for the log.
int hfog_stats(u8 peak[3]);

// Into the current target through `vp`, once an eye. Fog must be off, as it
// is after the cars. Puts blend, depth and the TEV back the way model3d.c
// leaves them.
void hfog_draw(const C3D_Mtx* vp);
