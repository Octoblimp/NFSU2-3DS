#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>

// Runtime loader for the frontend/global ".tpk" texture-pack variant embedded
// in PC Files/GLOBAL/GLOBALB.BUN (Global\GlobalTextures.tpk /
// Global\GlobalTexturesPC.tpk). C port of tools/extract_global_textures.py --
// see that file and docs/formats/texture-tpk.md's "Frontend/global variant"
// section for the full on-disk format writeup this implements (0xb3300000
// container -> 0xb3310000 info + 0xb3320000 raw-DXT pixel-leaf sub-containers,
// 0x33310004/124-byte info records + parallel 0x33310005/32-byte format
// records, texture byte length recovered by sorting pixel offsets since the
// format stores no explicit length field).
//
// Unlike the car/track TEXTURES.BIN variant, this one is NOT JDLZ/HUFF
// wrapped -- pixel data is raw DXT1/DXT3 block bytes, decoded directly.

// Format record (+0x14, u32) codes seen in practice: ASCII "DXT1"/"DXT3"
// literals, numeric 0x29 (D3DFMT_P8, 8-bit palettized -- see TEXFMT_P8), and
// numeric 0x15 which
// byte-size math (format-record field +0x40 == width*height*4 exactly, not
// any DXT block-count formula) confirms is raw uncompressed pixel data, not
// DXT1 -- verified against PC_LOAD/HEADLIGHTS/RAINDROPPC by decoding their
// raw bytes as BGRA8888 and visually confirming a coherent image (PC_LOAD's
// severe on-screen corruption was this: it was being read as DXT1 blocks).
typedef enum {
	TEXFMT_DXT1,
	TEXFMT_DXT3,
	TEXFMT_RAW_BGRA8888,
	// Format code 0x29 = D3DFMT_P8: one byte per pixel, indexing a 256-entry
	// BGRA8888 CLUT. The CLUTs are packed together at the HEAD of the pixel
	// leaf -- no info record points at one; a record only declares its palette's
	// byte length at +0x3C (0x400 on every palettized record, 0 on every other)
	// and the position is implied by record order.
	//
	// NOT one shared palette per leaf, as this comment used to claim: that held
	// for GLOBALB.BUN's U2_ICON_HIGHLIGHT and FrontB.lzc's
	// OPTIONS_ICON_BURNOUT_DE only because those containers happen to carry a
	// single palettized texture each, so palette 0 was theirs. L4RA's Y0
	// library carries 21 -- the road, kerb and sidewalk art the whole city is
	// paved with -- and under the old rule all 21 would have drawn through the
	// first one's colours.
	//
	// Located by elimination: after laying every one of Y0's 1032 textures down
	// at its recorded offset for its computed run length, the only unclaimed
	// region of the leaf is the first 21504 bytes = 21 x 1024, against exactly
	// 21 palettized records. Pairing the k-th such record with the k-th CLUT
	// renders all 21 as coherent road art; any other pairing renders noise.
	TEXFMT_P8,
} TexPixFormat;

typedef struct {
	u32 hash;
	u16 width, height;
	TexPixFormat format;
	const u8* pixelData; // points into TexPack.fileBuf -- valid only while the pack is loaded
	u32 pixelLen;
	// TEXFMT_P8 only: this entry's own 256-entry BGRA8888 CLUT, inside the CLUT
	// block at the head of its container's pixel leaf. NULL for every other
	// format, and NULL for a P8 entry whose CLUT block does not fit in front of
	// the first texture's pixels -- dropping it rather than drawing through
	// some other texture's colours.
	const u8* palette;
	// texpack_load() only. Its entries leave pixelData and palette NULL and
	// say where the bytes are in the open file instead; they are read when the
	// entry is decoded. srcPalOff is 0 when the entry has no CLUT (a real one
	// can never start at file offset 0).
	FILE* src;
	u32 srcOff, srcPalOff;
} TexPackEntry;

typedef struct {
	u8* fileBuf;
	size_t fileSize;
	TexPackEntry* entries;
	int count;
	FILE* fp; // texpack_load()'s open file, closed by texpack_free()
} TexPack;

// DIAGNOSTIC (#144): why the last texpack_load()/texpack_load_mem() returned
// false. Both used to answer a failed 8MB malloc, a missing file and a
// container scan that found nothing with the same bare `false`, which is what
// left TASKS.md #144 unable to say which one was happening.
enum {
	TEXPACK_FAIL_NONE = 0,
	TEXPACK_FAIL_OPEN,            // fopen() refused
	TEXPACK_FAIL_SIZE,            // ftell() <= 0
	TEXPACK_FAIL_MALLOC_FILE,     // whole-file malloc refused  (size = bytes asked)
	TEXPACK_FAIL_READ,            // short fread()
	TEXPACK_FAIL_CALLOC_ENTRIES,  // entry-table calloc refused (size = bytes asked)
	TEXPACK_FAIL_NO_CONTAINERS,   // parsed fine, found zero textures
};

typedef struct {
	int stage;
	u32 size;
	int count;
} TexPackFail;

extern TexPackFail g_texpackFail;

// Indexes every 0xb3300000 container in `path` (GLOBALB.BUN has two, at
// non-zero byte offsets) without reading the file into memory: the chunk
// headers are walked with seeks, each container's info records are read, and
// the pixel bytes stay on the card until an entry is decoded. The file stays
// open until texpack_free(). GLOBALB.BUN is 8MB, and reading it whole for every
// screen asked the heap for one 8MB block -- which the world's resident
// sections leave unavailable often enough that an in-world dialog would come
// up with every chrome texture missing on one run and all of them on the next.
// A file whose containers the chunk walk cannot reach is read whole as before.
// Returns false if the file can't be opened or no container is found.
bool texpack_load(const char* path, TexPack* out);

// Same container scan, but over a caller-owned buffer that is already in
// memory and already decompressed. Needed for FRONTEND/FrontB.lzc: its outer
// JDLZ block holds not just the 130 frontend .fng screens but two texture
// packs of its own -- Global\FrontEndTextures.tpk (495 textures) and
// Global\FrontEndTexturesPC.tpk (3) -- which is where every frontend-only
// texture lives that GLOBALB.BUN's Global\GlobalTextures.tpk does not have.
// Confirmed by scanning the decompressed FrontB.lzc with
// tools/extract_global_textures.py's find_containers(): UI_Main.fng's seven
// MAIN_ICON_* menu icons and U2_MENU_LOGO (0x7609B60A -- the texture TASKS.md
// #83 had recorded as "genuinely absent" from GLOBALA/GLOBALB) are all in
// there. `out->fileBuf` is left NULL: the buffer stays the caller's to free,
// so entries' pixelData pointers are only valid while it lives.
bool texpack_load_mem(const u8* buf, size_t len, TexPack* out);

void texpack_free(TexPack* t);

// texpack_load(), remembered (TASKS.md #270): the first call for a path loads
// it and later ones hand back the same index -- GLOBALB's alone was 122 ms of
// every screen load. Only a pack texpack_load() streamed is kept, i.e. an
// index and an open file but no pixels; one it had to read whole is loaded
// fresh every time, as before, rather than holding megabytes of malloc heap.
// Either way, give it back with texpack_release(), never texpack_free().
bool texpack_load_shared(const char* path, TexPack* out);
void texpack_release(TexPack* t);

// Linear scan by hash (a few hundred entries -- fine for the handful of
// lookups a screen's Image widgets do once at load time).
const TexPackEntry* texpack_find(const TexPack* t, u32 hash);

// Decodes one entry straight into a caller-owned GPU texture, bypassing
// TexCache entirely. TexCache never evicts -- it is sized for the fixed set of
// textures a screen's widgets reference -- so anything that has to be swapped
// repeatedly while a screen is up (the per-track map, which is a whole 512x512
// texture per selection) needs its own slot it can delete and refill. On
// success the caller owns `tex` and must C3D_TexDelete() it.
//
// `maxDim`, when > 0, box-filters the decoded image down by successive halves
// until neither axis exceeds it. This is a real reduction in resident memory,
// not a fallback: see trackmap.h.
//
// `opaqueIfAlphaEmpty` promotes the whole image to alpha 255 when every texel
// decoded to alpha 0. The track maps need it: they are DXT3 (their
// 0x33310005 record's +0x08 is 1, same as a genuinely translucent DXT3 like
// U2_MENU_BOX) but their alpha blocks are empty -- verified across all 129
// shipped TRACKMAP*.BIN, and the consequence on hardware is a completely
// invisible texture over the authored panel. Not applied globally, because
// on any other texture "alpha 0 everywhere" has never been observed and a
// blanket promotion would change screens that are already verified correct.
bool texpack_decode_entry(const TexPackEntry* e, int maxDim, bool opaqueIfAlphaEmpty,
	C3D_Tex* tex, Tex3DS_SubTexture* subtex);

// texpack_decode_entry(), with the decoded alpha rescaled so the texture's own
// peak becomes 255. MINIMAP_MASK is the one user (TASKS.md #287): retail
// authors its circle at alpha 204, which is not the shape but the translucency
// the PC build composites the minimap over the 3D view with
// (psMiniMapAlphaMask, Speed.elf 0x801e6640). The port draws the minimap on a
// panel with nothing behind it and wants the circle alone, so the 204 would
// only dim the map art by a fifth. Rescaling by the peak keeps the antialiased
// rim's gradient intact, which a threshold would not.
bool texpack_decode_entry_alpha_full(const TexPackEntry* e, C3D_Tex* tex,
	Tex3DS_SubTexture* subtex);

// A small fixed-size cache of decoded GPU textures, keyed by resource hash --
// avoids re-decoding/re-uploading the same texture for multiple widgets that
// reference it, and lets the raw TexPack (and its ~8MB file buffer) be freed
// once every texture a screen actually uses has been decoded.
// Most textures a screen draws are textures one of its own widgets references
// (a backdrop included -- see main.c's UG_LS_Splash.fng notes), and 48 covered
// the largest screen by that measure (UI_Main.fng, 55 widgets but far fewer
// distinct hashes). UI_QRCarSelect is the screen that needs more: its two
// artwork slots are re-textured per selection by the engine (SPEED2.EXE
// 0x4B2310), so the cache has to hold every car's badge and every distinct
// manufacturer logo at once -- 29 + 19 on the shipped US data -- on top of the
// screen's own widget textures. At 256x64 and 128x64 RGBA8888 that is ~2.5MB
// of linear memory, warmed once at load because the pack those textures live
// in (FrontB.lzc, 6MB decompressed) is freed before the first scroll.
#define TEX_CACHE_MAX 128

typedef struct {
	u32 hash;
	C3D_Tex tex;
	Tex3DS_SubTexture subtex;
	C2D_Image image;
	bool valid;
	// #113: the pack entry it came from (size, format, pixel bytes), so the
	// cross-screen pool hands it out only for the same pixels. len 0 = not
	// from texcache_get(), never parked.
	struct { u16 w, h; u32 fmt, len; } key;
} TexCacheSlot;

typedef struct {
	TexCacheSlot slots[TEX_CACHE_MAX];
	int count;
} TexCache;

void texcache_init(TexCache* c);
// Parks the cache's textures in the cross-screen pool where it can (#113),
// deletes the rest.
void texcache_free(TexCache* c);

// #113: the pool of textures freed screens left behind. Off (and emptied)
// while a world is open; flush empties it.
void texcache_pool_enable(bool on);
void texcache_pool_flush(void);
void texcache_pool_stats(u32* hits, u32* misses, int* parked, u32* bytes);
// Textures that went into the world arena because the ordinary linear heap
// was full, and ones that fit nowhere (upload_rgba_tiled), since boot.
u32 texpack_arena_uploads(void);
u32 texpack_upload_fails(void);
// #437: textures uploaded as RGB8 (opaque, unpadded) since boot, and the KB
// that saved against RGBA8.
void texpack_rgb8_stats(u32* uploads, u32* savedKB);

// Decodes (DXT->RGBA8, then PICA200 8x8-tile swizzle + GPU upload, same
// scheme as vp6_display.c's video textures) and caches the texture for
// `hash`, looking it up in `pack` first. Returns a zero image
// ({NULL,NULL} tex/subtex) if not found in the pack, decode fails, or the
// cache is full -- callers must check img.tex != NULL before drawing.
C2D_Image texcache_get(TexCache* c, const TexPack* pack, u32 hash);

// Looks up an already-cached texture without a TexPack (for use after the
// pack's ~8MB file buffer has been freed post-load, once every texture a
// screen needs has been pre-warmed via texcache_get()). Returns a zero image
// ({NULL,NULL}) if `hash` was never cached.
C2D_Image texcache_find(const TexCache* c, u32 hash);

// One entry decoded to RGBA8 in main memory (row 0 at the top), for a texture
// the CPU reads rather than the GPU: rain.c's RAINDROPPC, whose channels are
// the lens drop's refraction (#406). malloc'd, the caller frees; NULL on a
// format the decoders do not take. `w` / `h` get its size.
u8* texpack_decode_rgba(const TexPackEntry* e, u16* w, u16* h);

// Uploads `w` x `h` RGBA8 (row 0 at the top) as a GPU texture, padded to the
// next power of two with `subtex` covering the used corner. For the port's
// own art rather than a pack's: data/buttons3ds.bin, the 3DS button sheet
// the frontend draws in each screen's $JOY_EVENT_FENG_*$ slot.
bool texpack_upload_rgba(const u8* rgba, u16 w, u16 h, C3D_Tex* tex,
	Tex3DS_SubTexture* subtex);
