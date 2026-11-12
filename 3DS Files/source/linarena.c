#include "linarena.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// libctru's own, reached through the link's --wrap (Makefile).
void* __real_linearAlloc(size_t size);
void* __real_linearMemAlign(size_t size, size_t alignment);
void  __real_linearFree(void* mem);
void  __wrap_linearFree(void* mem);

// linearAlloc()'s own alignment, kept for every block so a texture or a pool
// out of the arena is aligned exactly as one off the heap would be.
#define ARENA_ALIGN      0x80u
// Blocks, used and free, the arena can be cut into. A full world holds about
// 1500 textures and two pools for each of 60-odd sections, and every used
// block has at most one free one beside it, so this is twice the worst case.
#define ARENA_MAX_BLOCKS 8192
// Windows arena_make_run() weighs before it gives up on the cheapest: the
// cheapest one can be impossible (what is in it has nowhere else to go) while
// the next is not.
#define RUN_CANDIDATES   6
// Blocks one window may ask to move. A window that would need more is not a
// window worth emptying.
#define RUN_MAX_MOVES    256

// The arena as a list of blocks sorted by offset that covers it end to end:
// a free block's neighbours are always used ones (freeing merges), so the
// list is never longer than twice the live allocations plus one.
typedef struct {
	u32 off;
	u32 len;
	u32 tag;    // the owner arena_tag() named, 0 = cannot be moved
	u8  used;   // 0 free, else the ArenaSide it was placed from
} Block;

static u8*    s_base;
static u32    s_size;
static u32    s_used;
static Block* s_blk;
static int    s_count;
static int    s_live;
// arena_make_run()'s working space, allocated with the block list.
static u32*   s_hole;   // free stretches outside a window
static u8*    s_mov;    // per block: free, or used and movable
// Per thread: the world's main-thread allocations are routed, and an audio
// buffer allocated on another thread in the same instant is not.
static __thread u8  s_route;
static __thread u32 s_tag;

static u32 alignUp(u32 v, u32 a) {
	return (v + a - 1) & ~(a - 1);
}

static void insertAt(int i, Block b) {
	memmove(&s_blk[i + 1], &s_blk[i], (size_t)(s_count - i) * sizeof(Block));
	s_blk[i] = b;
	s_count++;
}

static void removeAt(int i) {
	memmove(&s_blk[i], &s_blk[i + 1], (size_t)(s_count - i - 1) * sizeof(Block));
	s_count--;
}

// The block starting at `off`, or -1.
static int findBlock(u32 off) {
	int lo = 0, hi = s_count - 1;
	while (lo <= hi) {
		const int mid = (lo + hi) / 2;
		if (s_blk[mid].off == off)
			return mid;
		if (s_blk[mid].off < off)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

// Where an allocation of `size` would go: the block index and the offset
// inside the arena, or -1. `skip`/`skipLen`, when skip >= 0, pretend block
// `skip` is only that long (arena_fits2's second pool, after the first).
static int place(u32 size, u32 align, int side, int skip, u32 skipLen, u32* start) {
	int pick = -1;
	if (side == ARENA_LOW) {
		for (int i = 0; i < s_count; i++) {
			const Block* b = &s_blk[i];
			const u32 len = i == skip ? skipLen : b->len;
			if (b->used || len < size)
				continue;
			const u32 st = alignUp(b->off, align);
			if (st + size <= b->off + len) {
				*start = st;
				return i;
			}
		}
		return -1;
	}
	// Best fit, and the highest of equals: a pool goes in the smallest hole
	// that holds it, at the hole's top, so what is left of the hole stays in
	// one piece below it.
	u32 bestLen = 0;
	for (int i = s_count - 1; i >= 0; i--) {
		const Block* b = &s_blk[i];
		const u32 len = i == skip ? skipLen : b->len;
		if (b->used || len < size)
			continue;
		const u32 st = (b->off + len - size) & ~(align - 1);
		if (st < b->off)
			continue;
		if (pick < 0 || len < bestLen) {
			pick = i;
			bestLen = len;
			*start = st;
		}
	}
	return pick;
}

// Cuts [st, st + size) out of free block i; what is left either side stays
// free.
static void cut(int i, u32 st, u32 size, u8 side, u32 tag) {
	const Block b = s_blk[i];
	const u32 headLen = st - b.off;
	const u32 tailOff = st + size;
	const u32 tailLen = b.off + b.len - tailOff;
	int at = i;
	if (headLen) {
		s_blk[at].len = headLen;
		insertAt(++at, (Block){ st, size, tag, side });
	} else {
		s_blk[at] = (Block){ st, size, tag, side };
	}
	if (tailLen)
		insertAt(at + 1, (Block){ tailOff, tailLen, 0, 0 });
	s_used += size;
	s_live++;
}

static void* arenaAlloc(u32 size, u32 align, int side) {
	if (!size)
		size = 1;
	if (align < ARENA_ALIGN)
		align = ARENA_ALIGN;
	size = alignUp(size, ARENA_ALIGN);
	if (s_count + 2 > ARENA_MAX_BLOCKS)
		return NULL;
	u32 st = 0;
	const int i = place(size, align, side, -1, 0, &st);
	if (i < 0)
		return NULL;
	cut(i, st, size, (u8)side, s_tag);
	return s_base + st;
}

static void arenaFree(void* p) {
	const int i = findBlock((u32)((u8*)p - s_base));
	if (i < 0 || !s_blk[i].used)
		return;   // not the start of a live block: nothing of ours
	s_blk[i].used = 0;
	s_blk[i].tag = 0;
	s_used -= s_blk[i].len;
	s_live--;
	if (i + 1 < s_count && !s_blk[i + 1].used) {
		s_blk[i].len += s_blk[i + 1].len;
		removeAt(i + 1);
	}
	if (i > 0 && !s_blk[i - 1].used) {
		s_blk[i - 1].len += s_blk[i].len;
		removeAt(i);
	}
}

static bool owns(const void* p) {
	return s_base && (const u8*)p >= s_base && (const u8*)p < s_base + s_size;
}

bool arena_create(u32 bytes) {
	if (s_base)
		return false;
	bytes &= ~(ARENA_ALIGN - 1);
	if (!bytes)
		return false;
	s_blk  = (Block*)malloc(sizeof(Block) * ARENA_MAX_BLOCKS);
	s_hole = (u32*)malloc(sizeof(u32) * (ARENA_MAX_BLOCKS + 2));
	s_mov  = (u8*)malloc(ARENA_MAX_BLOCKS);
	if (s_blk && s_hole && s_mov)
		s_base = (u8*)__real_linearAlloc(bytes);
	if (!s_base) {
		free(s_blk);
		free(s_hole);
		free(s_mov);
		s_blk = NULL;
		s_hole = NULL;
		s_mov = NULL;
		return false;
	}
	s_size = bytes;
	s_used = 0;
	s_live = 0;
	s_blk[0] = (Block){ 0, bytes, 0, 0 };
	s_count = 1;
	return true;
}

void arena_destroy(void) {
	if (!s_base)
		return;
	__real_linearFree(s_base);
	free(s_blk);
	free(s_hole);
	free(s_mov);
	s_base = NULL;
	s_blk = NULL;
	s_hole = NULL;
	s_mov = NULL;
	s_size = s_used = 0;
	s_count = s_live = 0;
}

bool arena_active(void) {
	return s_base != NULL;
}

ArenaSide arena_route(ArenaSide side) {
	const ArenaSide was = (ArenaSide)s_route;
	s_route = (u8)side;
	return was;
}

u32 arena_tag(u32 tag) {
	const u32 was = s_tag;
	s_tag = tag;
	return was;
}

u32 arena_size(void) {
	return s_size;
}

u32 arena_used(void) {
	return s_used;
}

int arena_blocks(void) {
	return s_live;
}

u32 arena_largest(void) {
	u32 best = 0;
	for (int i = 0; i < s_count; i++)
		if (!s_blk[i].used && s_blk[i].len > best)
			best = s_blk[i].len;
	return best;
}

bool arena_fits2(u32 a, u32 b) {
	if (!s_base)
		return true;
	a = alignUp(a ? a : 1, ARENA_ALIGN);
	b = alignUp(b ? b : 1, ARENA_ALIGN);
	u32 st = 0;
	const int i = place(a, ARENA_ALIGN, ARENA_HIGH, -1, 0, &st);
	if (i < 0)
		return false;
	// The first pool takes the top of its block; what is below it is free.
	return place(b, ARENA_ALIGN, ARENA_HIGH, i, st - s_blk[i].off, &st) >= 0;
}

u32 arena_run_if_freed(const void* p, const void* q) {
	if (!s_base)
		return 0;
	const int ip = p && owns(p) ? findBlock((u32)((const u8*)p - s_base)) : -1;
	const int iq = q && owns(q) ? findBlock((u32)((const u8*)q - s_base)) : -1;
	u32 best = arena_largest();
	const int t[2] = { ip, iq };
	for (int k = 0; k < 2; k++) {
		if (t[k] < 0)
			continue;
		int lo = t[k], hi = t[k];
		while (lo > 0 && (!s_blk[lo - 1].used || lo - 1 == ip || lo - 1 == iq))
			lo--;
		while (hi + 1 < s_count && (!s_blk[hi + 1].used || hi + 1 == ip || hi + 1 == iq))
			hi++;
		const u32 run = s_blk[hi].off + s_blk[hi].len - s_blk[lo].off;
		if (run > best)
			best = run;
	}
	return best;
}

// ------------------------------------------------------------- compaction

// The free stretches outside [lo, hi): of each free block, the part below the
// window and the part above it. Into s_hole; returns how many.
static int holesOutside(u32 lo, u32 hi) {
	int n = 0;
	for (int i = 0; i < s_count; i++) {
		const Block* b = &s_blk[i];
		if (b->used)
			continue;
		const u32 end = b->off + b->len;
		if (b->off < lo)
			s_hole[n++] = (end < lo ? end : lo) - b->off;
		if (end > hi)
			s_hole[n++] = end - (b->off > hi ? b->off : hi);
	}
	return n;
}

// Whether every block in `len` (largest first) finds a stretch outside the
// window, placed best fit one after another the way moveOut() will.
static bool fitsOutside(u32 lo, u32 hi, const u32* len, int n) {
	const int nh = holesOutside(lo, hi);
	for (int k = 0; k < n; k++) {
		int pick = -1;
		for (int h = 0; h < nh; h++)
			if (s_hole[h] >= len[k] && (pick < 0 || s_hole[h] < s_hole[pick]))
				pick = h;
		if (pick < 0)
			return false;
		s_hole[pick] -= len[k];
	}
	return true;
}

// The used blocks overlapping [lo, hi), largest first: offsets into `off`,
// lengths into `len`. -1 when there are more than `max`.
static int blocksIn(u32 lo, u32 hi, u32* off, u32* len, int max) {
	int n = 0;
	for (int i = 0; i < s_count && s_blk[i].off < hi; i++) {
		const Block* b = &s_blk[i];
		if (!b->used || b->off + b->len <= lo)
			continue;
		if (n == max)
			return -1;
		int k = n++;
		while (k > 0 && len[k - 1] < b->len) {
			len[k] = len[k - 1];
			off[k] = off[k - 1];
			k--;
		}
		len[k] = b->len;
		off[k] = b->off;
	}
	return n;
}

// Moves the block at `from` out of [lo, hi) into the smallest free stretch
// outside it -- at the stretch's top for a pool, its bottom for a texture,
// as each side places -- and frees where it was. False when nothing fits.
static bool moveOut(u32 from, u32 lo, u32 hi, const ArenaMover* mv) {
	const int src = findBlock(from);
	if (src < 0 || !s_blk[src].used || s_count + 2 > ARENA_MAX_BLOCKS)
		return false;
	const Block b = s_blk[src];
	int pick = -1;
	u32 bestLen = 0, st = 0;
	for (int i = 0; i < s_count; i++) {
		const Block* f = &s_blk[i];
		if (f->used)
			continue;
		const u32 end = f->off + f->len;
		for (int part = 0; part < 2; part++) {
			const u32 a = part ? (f->off > hi ? f->off : hi) : f->off;
			const u32 z = part ? end : (end < lo ? end : lo);
			if (z <= a || z - a < b.len)
				continue;
			if (pick < 0 || z - a < bestLen) {
				pick = i;
				bestLen = z - a;
				st = b.used == ARENA_HIGH ? z - b.len : a;
			}
		}
	}
	if (pick < 0)
		return false;
	cut(pick, st, b.len, b.used, b.tag);
	void* dst = s_base + st;
	void* old = s_base + from;
	memcpy(dst, old, b.len);
	GSPGPU_FlushDataCache(dst, b.len);
	mv->moved(b.tag, old, dst, mv->user);
	arenaFree(old);
	return true;
}

ArenaRun arena_make_run(u32 need, u32 maxBytes, const ArenaMover* mv, u32* movedOut) {
	if (movedOut)
		*movedOut = 0;
	if (!s_base)
		return ARENA_RUN_READY;
	need = alignUp(need ? need : 1, ARENA_ALIGN);
	if (arena_largest() >= need)
		return ARENA_RUN_READY;
	if (need > s_size || s_size - s_used < need)
		return ARENA_RUN_NONE;

	for (int i = 0; i < s_count; i++)
		s_mov[i] = !s_blk[i].used ||
			(s_blk[i].tag && mv->canMove(s_blk[i].tag, s_base + s_blk[i].off, mv->user));

	// The cheapest windows `need` long, by bytes to move: every one starting
	// where a block starts and every one ending where a block ends.
	u32 cLo[RUN_CANDIDATES], cCost[RUN_CANDIDATES];
	int nc = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < s_count; i++) {
			u32 lo, hi;
			if (pass == 0) {
				lo = s_blk[i].off;
				if (lo + need > s_size)
					break;
				hi = lo + need;
			} else {
				hi = s_blk[i].off + s_blk[i].len;
				if (hi < need)
					continue;
				lo = hi - need;
			}
			const u32 worst = nc == RUN_CANDIDATES ? cCost[nc - 1] : 0xffffffffu;
			u32 cost = 0;
			bool ok = true;
			// From the block holding `lo` up to the one holding `hi - 1`.
			int k = i;
			if (pass == 1)
				while (k > 0 && s_blk[k].off > lo)
					k--;
			for (; k < s_count && s_blk[k].off < hi; k++) {
				if (s_blk[k].off + s_blk[k].len <= lo || !s_blk[k].used)
					continue;
				if (!s_mov[k]) {
					ok = false;
					break;
				}
				cost += s_blk[k].len;
				if (cost >= worst) {
					ok = false;
					break;
				}
			}
			if (!ok)
				continue;
			int at = nc < RUN_CANDIDATES ? nc++ : nc - 1;
			while (at > 0 && cCost[at - 1] > cost) {
				cCost[at] = cCost[at - 1];
				cLo[at] = cLo[at - 1];
				at--;
			}
			cCost[at] = cost;
			cLo[at] = lo;
		}
	}

	static u32 off[RUN_MAX_MOVES], len[RUN_MAX_MOVES];
	for (int c = 0; c < nc; c++) {
		const u32 lo = cLo[c], hi = cLo[c] + need;
		const int n = blocksIn(lo, hi, off, len, RUN_MAX_MOVES);
		if (n <= 0 || !fitsOutside(lo, hi, len, n))
			continue;
		u32 moved = 0;
		for (int k = 0; k < n; k++) {
			if (moved && moved + len[k] > maxBytes)
				break;
			if (!moveOut(off[k], lo, hi, mv))
				break;
			moved += len[k];
		}
		if (movedOut)
			*movedOut = moved;
		if (arena_largest() >= need)
			return ARENA_RUN_READY;
		return moved ? ARENA_RUN_MOVING : ARENA_RUN_NONE;
	}
	return ARENA_RUN_NONE;
}

// ---------------------------------------------------------------- the wrap

void* __wrap_linearAlloc(size_t size) {
	if (s_route != ARENA_OFF && s_base)
		return arenaAlloc((u32)size, ARENA_ALIGN, s_route);
	return __real_linearAlloc(size);
}

void* __wrap_linearMemAlign(size_t size, size_t alignment) {
	if (s_route != ARENA_OFF && s_base)
		return arenaAlloc((u32)size, (u32)alignment, s_route);
	return __real_linearMemAlign(size, alignment);
}

// #521 round 4: frees held back while the GPU is still drawing from the heap
// (linarena_hold_frees). The sim a frame ahead runs with this frame's kicked
// lists in flight, and a block it frees may be one they read.
#define HOLD_MAX 256
static void* s_hold[HOLD_MAX];
static int   s_holdN;
static bool  s_holdOn;

void linarena_hold_frees(bool on) {
	s_holdOn = on;
}

int linarena_release_held(void) {
	const int n = s_holdN;
	s_holdN = 0;
	for (int i = 0; i < n; i++)
		__wrap_linearFree(s_hold[i]);
	return n;
}

void __wrap_linearFree(void* mem) {
	if (s_holdOn && mem && s_holdN < HOLD_MAX) {
		s_hold[s_holdN++] = mem;
		return;
	}
	if (owns(mem)) {
		arenaFree(mem);
		return;
	}
	__real_linearFree(mem);
}
