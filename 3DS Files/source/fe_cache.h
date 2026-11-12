// A frontend room's picture, kept from one frame to the next (TASKS.md #319).
// The rooms are fixed shots -- both cameras are built once, the rooms are
// opaque and baked, and nothing in them scrolls -- so only the turntable
// moves, with the car on it. The room without its turntable
// (model3d_draw_scene_part) is drawn once into its screen's target and its
// colour and depth copied aside; every frame after that, the GPU's transfer
// engine puts the copy back, and the turntable and the car are drawn over it
// as before. On the card the room was 5.8 ms of GPU a frame on the top screen
// and 5.6 on the bottom, for 6,360 triangles.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

typedef enum {
	FECACHE_TOP,      // fe_post.c's scene target
	FECACHE_BOTTOM,   // the lower panel's own
	FECACHE_SLOTS
} FeCacheSlot;

// What a picture depends on, compared whole. `gen` is the caller's count of
// rooms loaded, so a room loaded where the last one was freed is not taken
// for it. Build it from a zeroed struct: the padding is compared too.
typedef struct {
	const void* scene;
	u32 gen;
	C3D_Mtx vp;
	float ambient, gain, invGamma;
	const C3D_RenderTarget* target;
} FeCacheKey;

// True when `slot` holds the picture drawn under `key`: fecache_restore()
// then stands in for the clear and the room.
bool fecache_hit(FeCacheSlot slot, const FeCacheKey* key);

// Queues the copy of `slot`'s picture back into `t`, the whole of its colour
// and depth buffers, after the GPU's commands so far.
void fecache_restore(FeCacheSlot slot, C3D_RenderTarget* t);

// Queues a copy of `t` as its commands so far leave it, as `slot`'s picture
// under `key`. The VRAM is taken on the first call and kept; false, logged
// once, without it -- the room is then drawn every frame, as before.
bool fecache_store(FeCacheSlot slot, C3D_RenderTarget* t, const FeCacheKey* key,
	void (*log)(const char*));

// Forgets every picture, keeping the VRAM: for the next frame to draw its
// rooms afresh.
void fecache_drop(void);
// #352: forgets them and gives their VRAM back, for the world (fe_post.h's
// fepost_release()); the next fecache_store() makes it again.
void fecache_release(void);
