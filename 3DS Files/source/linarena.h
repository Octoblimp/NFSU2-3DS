#ifndef NFSU2_LINARENA_H
#define NFSU2_LINARENA_H

#include <3ds.h>

// The world's own piece of the linear heap (TASKS.md #270).
//
// The streamed world frees and re-allocates linear memory for the whole drive:
// a section's vertex and index pools (50 KB to 6 MB each) and its share of the
// texture bank (2 to 171 KB per texture), interleaved on one heap with
// everything else. libctru's allocator is first-fit over all of it, and after
// a few minutes of that the heap is in pieces: on 2026-10-25 a race had 23 MB
// of linear memory free and no single block over 2.7 MB, so A32's vertex pool
// did not allocate, and the run before it eight A sections never loaded at
// all -- the user's "holes that never loaded".
//
// So the world gets one block, taken once at world_open() and sized to its
// budget, and allocates inside it from both ends: textures first-fit from the
// bottom, where same-sized ones reuse each other's holes, and geometry pools
// best-fit from the top, where a freed pool leaves a gap the next one fits.
// What the world holds can never eat into what the overlays and the race were
// promised -- it stops at the arena's edge -- and what breaks up inside it
// anyway is put back together by moving blocks (compaction, below). Retail's TrackStreamer has a pool of its own for the
// same reasons (MakeSpaceInPool 0x802065E4 works on its largest free block).
//
// Nothing calls into the arena directly: the link wraps libctru's
// linearAlloc/linearMemAlign/linearFree (Makefile, -Wl,--wrap), and the world
// says which end the next allocations on this thread come from with
// arena_route(). citro3d's C3D_TexInitWithParams() and model3d's pools then
// land in the arena without either knowing; linearFree() finds them by
// address. Main thread only, like the allocator it stands in for.

typedef enum {
	ARENA_OFF = 0,   // the ordinary linear heap (the default)
	ARENA_LOW,       // the arena, first fit from the bottom: textures
	ARENA_HIGH,      // the arena, best fit from the top: geometry pools
} ArenaSide;

// Takes `bytes` of linear memory as the arena. False, with no arena, when the
// heap will not give one block that size.
bool arena_create(u32 bytes);
// Gives the block back. Anything still allocated in it is leaked -- logged by
// the caller through arena_used().
void arena_destroy(void);
bool arena_active(void);

// Where this thread's linear allocations go until the next call. Returns the
// previous setting, for nesting.
ArenaSide arena_route(ArenaSide side);

u32  arena_size(void);
u32  arena_used(void);
u32  arena_largest(void);      // the biggest free block
int  arena_blocks(void);       // allocations live in it

// Whether two allocations of these sizes would both fit right now, the way
// ARENA_HIGH would place them -- a section's vertex and index pools.
bool arena_fits2(u32 a, u32 b);
// The largest free run there would be if the blocks holding `p` and `q`
// (either may be NULL) were freed: what evicting one section would open up.
u32  arena_run_if_freed(const void* p, const void* q);

// ------------------------------------------------------------- compaction
//
// Two ends were not enough. On 2026-10-25 the arena itself was 12 MB free with
// no block over 1 MB after a few minutes of driving -- a texture freed in the
// middle of the pools, a pool freed in the middle of the textures -- and C22
// and B26, wanted, did not fit. Retail meets the same thing with hole filling
// after UnloadLRU (MakeSpaceInPool 0x802065E4); this is that: blocks move.
//
// A block can move when its owner said who it is (arena_tag(), set around the
// allocation the way arena_route() is) and says now that nothing is writing
// it. arena_make_run() picks the window of the arena that is cheapest to
// empty, copies each block in it to a free stretch outside it, flushes the
// copy for the GPU, and has the owner repoint at the copy before the old
// block is freed. Main thread only, between frames: the GPU must not be
// reading what moves, which world_update() can promise because C3D_FrameBegin
// waited for the last frame and this frame has drawn nothing yet. Every block
// is assumed 0x80-aligned, which is all linearAlloc() gives and all citro3d
// and model3d ask for.

// The owner tag the next allocations on this thread carry, 0 for none (such
// a block never moves). Returns the previous one, for nesting.
u32 arena_tag(u32 tag);

typedef struct {
	// Whether the block at `p`, owned by `tag`, may move right now -- and is
	// still what the owner holds, which a stale tag would not be.
	bool (*canMove)(u32 tag, const void* p, void* user);
	// The block owned by `tag` now lives at `to`; its bytes are already there
	// and flushed. Repoint at it -- `from` is freed when this returns.
	void (*moved)(u32 tag, void* from, void* to, void* user);
	void* user;
} ArenaMover;

typedef enum {
	ARENA_RUN_READY = 0,   // a free run of `need` exists
	ARENA_RUN_MOVING,      // blocks moved, the budget ran out first: call again
	ARENA_RUN_NONE,        // not with what is free and movable now
} ArenaRun;

// Moves blocks until one free run is at least `need` long, copying no more
// than `maxBytes` this call (the first block always moves, whatever its size).
// `*moved`, when given, gets what was copied.
ArenaRun arena_make_run(u32 need, u32 maxBytes, const ArenaMover* mv, u32* moved);

#endif

// #521 round 4: while on, linearFree() only notes the block; the blocks are
// freed by linarena_release_held(), called once the GPU has finished the frame
// that may still read them (right after the main loop's C3D_FrameBegin()).
// Returns how many it freed.
void linarena_hold_frees(bool on);
int  linarena_release_held(void);
