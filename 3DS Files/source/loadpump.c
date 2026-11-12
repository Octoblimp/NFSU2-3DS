#include "loadpump.h"

#include <stdio.h>

#include "linarena.h"

// See loadpump.h.

#define TICKS_PER_MS (SYSCLOCK_ARM11 / 1000)

// Reads past this are split, with a poll between the pieces: about 20 ms of
// SD time apiece, under one pumped frame's spacing.
#define LOADPUMP_READ_PIECE (64u << 10)

static void (*s_tick)(void*);
static void* s_ud;
static bool  s_busy;
static u64   s_last;      // svcGetSystemTick() of the last frame, 0 for none yet
static int   s_frames;
static u64   s_longest;   // in ticks
static u64   s_drawing;   // in ticks

void loadpump_arm(void (*tick)(void* ud), void* ud) {
	s_tick = tick;
	s_ud = ud;
	s_last = 0;
	s_frames = 0;
	s_longest = 0;
	s_drawing = 0;
}

bool loadpump_armed(void) {
	return s_tick != NULL;
}

void loadpump_mark(u64 frameTicks) {
	const u64 now = svcGetSystemTick();
	if (s_last && now - s_last > s_longest)
		s_longest = now - s_last;
	s_last = now;
	s_frames++;
	s_drawing += frameTicks;
}

void loadpump_poll(void) {
	if (!s_tick || s_busy)
		return;
	// libctru's threads carry their Thread; the main thread's is NULL. The
	// audio and section-reader threads read files too, and must never draw.
	if (threadGetCurrent() != NULL)
		return;
	if (s_last && svcGetSystemTick() - s_last < (u64)LOADPUMP_MS * TICKS_PER_MS)
		return;
	s_busy = true;
	// Whatever end of the world's arena the load was allocating from is the
	// load's, not the frame's (linarena.h).
	const ArenaSide was = arena_route(ARENA_OFF);
	s_tick(s_ud);
	arena_route(was);
	s_busy = false;
}

void loadpump_take_stats(int* frames, u32* longestMs, u32* drawMs) {
	if (frames)
		*frames = s_frames;
	if (longestMs)
		*longestMs = (u32)(s_longest / TICKS_PER_MS);
	if (drawMs)
		*drawMs = (u32)(s_drawing / TICKS_PER_MS);
	s_frames = 0;
	s_longest = 0;
	s_drawing = 0;
}

// The link's -Wl,--wrap=fread (Makefile): every fread() in the program comes
// here. Disarmed it is the real one; armed, a large read goes in pieces with a
// frame between them when one is due, and a small one is followed by a poll.
size_t __real_fread(void* buf, size_t size, size_t n, FILE* f);

size_t __wrap_fread(void* buf, size_t size, size_t n, FILE* f) {
	if (!s_tick || size == 0)
		return __real_fread(buf, size, n, f);
	if (size * n <= LOADPUMP_READ_PIECE || size > LOADPUMP_READ_PIECE) {
		const size_t got = __real_fread(buf, size, n, f);
		loadpump_poll();
		return got;
	}
	const size_t per = LOADPUMP_READ_PIECE / size;
	size_t done = 0;
	while (done < n) {
		const size_t want = n - done < per ? n - done : per;
		const size_t got = __real_fread((u8*)buf + done * size, size, want, f);
		done += got;
		loadpump_poll();
		if (got < want)
			break;
	}
	return done;
}
