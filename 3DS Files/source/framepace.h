// framepace.h -- how a world frame waits for the screen (TASKS.md #349, #352).
//
// C3D_FrameBegin(C3D_FRAME_SYNCDRAW) runs C3D_FrameSync(), which waits for a
// vblank that comes after the call (citro3d 1.7.1 spins on its two vblank
// counters until both have moved). A frame that takes 34 ms from one begin
// to the next therefore waits for the third vblank and is on screen for 50
// ms: the New 3DS perf tours' 40.9 ms average at J29.1 against a 28 ms GPU is
// that 20 / 30 fps mix.
//
// A paced frame begins with C3D_FrameBegin(0) -- only the last frame's GPU
// work is waited for -- and its C3D_FrameEnd() waits instead until the last
// frame's swap has been shown (gspIsPresentPending, both screens). That is
// what the vblank wait was there for: the display transfer must not write the
// framebuffer that is still on screen, and there is never more than one
// frame a vblank. A 34 ms frame is then shown at the next vblank after it is
// done, not at the one after that.
//
// Both calls are linker wraps (the Makefile's --wrap=C3D_FrameBegin,
// --wrap=C3D_FrameEnd), so every C3D_FrameEnd() -- the loading screens', the
// NIS starts', the movies' -- closes a paced frame the same way; a frame
// begun with C3D_FRAME_SYNCDRAW is left exactly as citro3d does it.
#pragma once

#include <3ds.h>

// 1 paces the world's frames on the present (config DbgFramePace=0 is the
// old C3D_FrameSync for the A/B; the look sweep's "vsync" step times it).
extern bool g_framePace;

// The time this window's frame ends spent waiting for the last swap, read
// and cleared by the profiler.
u64 framepace_wait_ticks_take(void);

// Successful C3D_FrameBegin()s so far: one that moved inside a main-loop frame
// means a loading screen or a scene start drew frames of its own in it.
u32 framepace_begun(void);

// linearFree(p) once the GPU can no longer touch it: at the next successful
// C3D_FrameBegin(). Inside a frame, citro3d's C3D_SyncDisplayTransfer() and
// C3D_SyncTextureCopy() only queue the copy -- it runs at C3D_FrameEnd() -- so
// a texture or buffer freed in the same frame is written or read after it has
// been handed to someone else (the boot movie's last frame over Loading.fng's
// splash, TASKS.md #439).
void framepace_free_after_gpu(void* p);
