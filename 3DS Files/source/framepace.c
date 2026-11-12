// framepace.c -- the world's frames paced on the present, not on a fresh
// vblank (framepace.h, TASKS.md #349, #352).
#include <3ds.h>
#include <citro3d.h>

#include "framepace.h"

bool g_framePace = true;

bool __real_C3D_FrameBegin(u8 flags);
void __real_C3D_FrameEnd(u8 flags);

static bool s_paced;       // the open frame was begun without C3D_FrameSync()
static u64  s_waitTicks;

// Linear blocks whose free waits for the GPU (framepace_free_after_gpu).
#define DEFER_MAX 16
static void* s_defer[DEFER_MAX];
static u32   s_deferStamp[DEFER_MAX];
static int   s_deferN;
static u32   s_begun;      // successful C3D_FrameBegin()s so far

// A successful C3D_FrameBegin() has waited for every command queued before it
// (citro3d's C3Di_WaitAndClearQueue), so a block handed over before this one
// began is no longer any queued transfer's source or destination.
static void reapDeferred(void)
{
	int k = 0;
	for (int i = 0; i < s_deferN; i++) {
		if (s_deferStamp[i] != s_begun)
			linearFree(s_defer[i]);
		else {
			s_defer[k] = s_defer[i];
			s_deferStamp[k++] = s_deferStamp[i];
		}
	}
	s_deferN = k;
}

void framepace_free_after_gpu(void* p)
{
	if (!p)
		return;
	if (s_deferN == DEFER_MAX) {
		// Never reached (the movie player hands over 7 at most); freeing now
		// is the old behaviour.
		linearFree(p);
		return;
	}
	s_defer[s_deferN] = p;
	s_deferStamp[s_deferN++] = s_begun;
}

bool __wrap_C3D_FrameBegin(u8 flags)
{
	s_paced = !(flags & C3D_FRAME_SYNCDRAW);
	const bool ok = __real_C3D_FrameBegin(flags);
	if (ok) {
		s_begun++;
		if (s_deferN)
			reapDeferred();
	}
	return ok;
}

void __wrap_C3D_FrameEnd(u8 flags)
{
	if (s_paced) {
		// The last frame's swap was asked for when its GPU work finished,
		// which this frame's C3D_FrameBegin(0) waited for; GSP shows it at
		// the next vblank. Until then the buffer this frame's display
		// transfer is about to write is the one on screen.
		const u64 t0 = svcGetSystemTick();
		while (gspIsPresentPending(GFX_TOP) || gspIsPresentPending(GFX_BOTTOM))
			gspWaitForAnyEvent();
		s_waitTicks += svcGetSystemTick() - t0;
		s_paced = false;
	}
	__real_C3D_FrameEnd(flags);
}

u32 framepace_begun(void)
{
	return s_begun;
}

u64 framepace_wait_ticks_take(void)
{
	const u64 t = s_waitTicks;
	s_waitTicks = 0;
	return t;
}
