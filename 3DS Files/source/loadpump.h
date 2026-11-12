#ifndef NFSU2_LOADPUMP_H
#define NFSU2_LOADPUMP_H

#include <3ds.h>

// The loading screen kept moving while a load blocks (TASKS.md #303).
//
// Retail loads on a thread of its own and draws Loading.fng every frame over
// it, so its five dots never stop (#302). Nothing here loads on a thread: world
// entry, world leave and a race's track switch run their steps straight
// through on the main thread, and used to draw the screen only between steps
// -- the dots stood still through world_open (1.5 s) and the GPS routes
// (1.1 s) on a career world entry.
//
// So while one of those loads is up, its frame is "pumped" from inside the
// load: every fread() on the main thread (the link wraps it, Makefile) and the
// loops that run long without one call loadpump_poll(), which draws a frame
// once LOADPUMP_MS has passed since the last. Large reads are split into
// pieces so a multi-megabyte bank cannot hold the screen for its whole length.
// A pumped frame costs its draw and nothing more: it skips SYNCDRAW's vblank
// wait, which the spacing makes unnecessary. Main thread only, and never from
// inside a frame (the tick sees C3D_FrameBegin() refuse and draws nothing).
// The page-in's own waits (world_prime(), the page-in loops in main.c) go
// through the pump too while it is armed.

#define LOADPUMP_MS 33

// Arms the pump with the loading screen's tick, or disarms it on NULL.
void loadpump_arm(void (*tick)(void* ud), void* ud);
bool loadpump_armed(void);

// A frame if one is due. Cheap when it is not, and a no-op off the main
// thread or with the pump disarmed.
void loadpump_poll(void);

// The tick calls this after each frame it draws, pumped or not, with what the
// frame took, so the pump spaces its own frames from the load's other ones.
void loadpump_mark(u64 frameTicks);

// Frames drawn, the longest wait between two of them and the time spent
// drawing them since the last call, for the load log.
void loadpump_take_stats(int* frames, u32* longestMs, u32* drawMs);

#endif
