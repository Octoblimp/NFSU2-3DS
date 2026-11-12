// A frontend room's picture, kept from one frame to the next (TASKS.md #319).
// See fe_cache.h.
//
// The copies are the transfer engine's TextureCopy, raw: a colour or depth
// buffer is 8x8 tiles laid out row by row, so a buffer the same size and
// format as the target is the target's bytes in the same order, and the copy
// is one run of them -- handed over a tile row (8 lines of pixels) at a time,
// with no gap. citro3d's C3D_SyncTextureCopy(), inside a frame, closes the
// command list off and queues the copy after it, which is the order wanted
// both ways: the room's commands before the copy taken of it, the copy put
// back before what is drawn over it. Closing the list also flushes and
// invalidates the GPU's framebuffer cache, so neither side reads stale tiles.
#include "fe_cache.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	bool valid;
	FeCacheKey key;
	void* colour;   // vramAlloc'd, the target's colour buffer's size
	void* depth;    // and its depth buffer's
	u32 colourBytes, depthBytes;
	u32 colourLine, depthLine;   // a tile row's bytes
} Slot;

static Slot s_slot[FECACHE_SLOTS];
static bool s_failed[FECACHE_SLOTS];

static u32 depthPixelBytes(GPU_DEPTHBUF fmt) {
	return fmt == GPU_RB_DEPTH16 ? 2 : fmt == GPU_RB_DEPTH24 ? 3 : 4;
}

static u32 colourPixelBytes(GPU_COLORBUF fmt) {
	return fmt == GPU_RB_RGBA8 ? 4 : fmt == GPU_RB_RGB8 ? 3 : 2;
}

// One copy, `bytes` in runs of `line`, queued after the commands so far.
static void copy(const void* from, void* to, u32 bytes, u32 line) {
	const u32 dim = GX_BUFFER_DIM(line >> 4, 0);
	// #481: the copy submits the draws so far itself, unflushed; on the 3DS
	// a running queue then reads the list before the cache has written it.
	C3D_FrameSplit(GX_CMDLIST_FLUSH);
	C3D_SyncTextureCopy((u32*)from, dim, (u32*)to, dim, bytes, GX_TRANSFER_RAW_COPY(1));
}

bool fecache_hit(FeCacheSlot slot, const FeCacheKey* key) {
	const Slot* c = &s_slot[slot];
	return c->valid && memcmp(&c->key, key, sizeof(*key)) == 0;
}

void fecache_restore(FeCacheSlot slot, C3D_RenderTarget* t) {
	const Slot* c = &s_slot[slot];
	if (!c->valid)
		return;
	copy(c->colour, t->frameBuf.colorBuf, c->colourBytes, c->colourLine);
	copy(c->depth, t->frameBuf.depthBuf, c->depthBytes, c->depthLine);
}

bool fecache_store(FeCacheSlot slot, C3D_RenderTarget* t, const FeCacheKey* key,
		void (*log)(const char*)) {
	Slot* c = &s_slot[slot];
	c->valid = false;
	if (s_failed[slot] || !t->frameBuf.colorBuf || !t->frameBuf.depthBuf)
		return false;
	const C3D_FrameBuf* fb = &t->frameBuf;
	const u32 cb = C3D_CalcColorBufSize(fb->width, fb->height, fb->colorFmt);
	const u32 db = C3D_CalcDepthBufSize(fb->width, fb->height, fb->depthFmt);
	if (c->colour && (c->colourBytes != cb || c->depthBytes != db)) {
		// Another target than the one it was made for: made again.
		vramFree(c->colour);
		vramFree(c->depth);
		c->colour = c->depth = NULL;
	}
	if (!c->colour) {
		const u32 before = vramSpaceFree();
		c->colour = vramAlloc(cb);
		c->depth = c->colour ? vramAlloc(db) : NULL;
		if (!c->depth) {
			if (c->colour)
				vramFree(c->colour);
			c->colour = NULL;
			s_failed[slot] = true;
			if (log) {
				char b[160];
				snprintf(b, sizeof(b), "fe_cache: no VRAM for the %s room's picture (%lu KB) -- "
					"it is drawn every frame", slot == FECACHE_TOP ? "top" : "bottom",
					(unsigned long)((cb + db) >> 10));
				log(b);
			}
			return false;
		}
		c->colourBytes = cb;
		c->depthBytes = db;
		c->colourLine = (u32)fb->width * 8 * colourPixelBytes(fb->colorFmt);
		c->depthLine = (u32)fb->width * 8 * depthPixelBytes(fb->depthFmt);
		if (log) {
			char b[160];
			snprintf(b, sizeof(b), "fe_cache: the %s room's picture kept, %ux%u: %lu KB VRAM "
				"(%lu KB left)", slot == FECACHE_TOP ? "top" : "bottom", fb->width, fb->height,
				(unsigned long)((before - vramSpaceFree()) >> 10),
				(unsigned long)(vramSpaceFree() >> 10));
			log(b);
		}
	}
	copy(fb->colorBuf, c->colour, cb, c->colourLine);
	copy(fb->depthBuf, c->depth, db, c->depthLine);
	c->key = *key;
	c->valid = true;
	return true;
}

void fecache_release(void) {
	for (int i = 0; i < FECACHE_SLOTS; i++) {
		Slot* c = &s_slot[i];
		if (c->colour)
			vramFree(c->colour);
		if (c->depth)
			vramFree(c->depth);
		c->colour = c->depth = NULL;
		c->valid = false;
	}
}

void fecache_drop(void) {
	for (int i = 0; i < FECACHE_SLOTS; i++)
		s_slot[i].valid = false;
}
