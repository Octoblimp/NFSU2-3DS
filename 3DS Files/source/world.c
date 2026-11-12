#include "world.h"
#include "lz4dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <errno.h>

#include <3ds.h>
#include <zlib.h>

#include "gpuwatch.h"
#include "heapcfg.h"
#include "loadpump.h"
#include "linarena.h"
#include "model3d.h"
#include "scenery.h"
#include "topology.h"

#define WORLD_MAGIC     0x5753464e   // 'NFSW' little-endian
#define WORLD_VERSION   2
#define WORLD_HDR       0x40
#define WORLD_REC       0x60
#define WORLD_NAME_LEN  16
#define WORLD_INDEX     "WORLD.NFSX"

// The shared texture bank, written alongside the sections by
// tools/track_pack.py (its TextureBank docstring has the layout and the
// reasoning). L4RA's sections reference 1562 distinct textures 20286 times
// between them, so pixels live here once instead of a dozen times over, and
// residency is refcounted: a road texture is uploaded when the first resident
// section wants it and freed when the last one drops. The card no longer
// carries WORLD.NFSB itself: WORLD.NFSZ holds a copy of its header and
// records, read as they are below, and every chain.
#define BANK_MAGIC      0x4253464e   // 'NFSB' little-endian
// Versions 2 and 3 are both read, and the range matters more than it looks:
//   2: the record's u16 at +0x0a carries each texture's TPK render state. A
//      version-1 bank has none, so every neon barrier and hanging light in it
//      would draw opaque -- which is why 1 is still refused.
//   3: the u16 at +0x14 carries maxLevel and `length` covers the whole mip
//      chain rather than level 0 alone.
// A v2 bank is read correctly by the v3 code path without a special case: its
// +0x14 is a reserved field the v2 packer zero-filled, maxLevel therefore
// reads 0, and chainBytes(l0, 0) is exactly l0. Checked rather than assumed --
// over 400 records of the shipped v2 bank, every byte of +0x14..0x1f is zero.
// So an older bank keeps working at its own resolution instead of drawing the
// whole city untextured grey, which is what requiring the exact version would
// have done to a half-migrated SD card.
#define BANK_VERSION_MIN 2
#define BANK_VERSION     3
#define BANK_HDR        0x40
#define BANK_REC        0x20
// The texture-animation table after the records (tools/track_pack.py, #326);
// its byte length is the header's +0x20, 0 in a bank packed before it.
#define BANK_ANIM_MAX_BYTES 4096
#define BANK_MAX_TEX    8192

// The track's sections and bank in one compressed file (#270 P2+P3+P7),
// written by tools/world_archive.py -- its docstring has the layout and the
// measurements behind it. Required since 2026-10-26 (the user's call, #270):
// it has to be there and built from this WORLD.NFSX, or the world does not
// open, and the loose section files and WORLD.NFSB are not read at all. It is
// opened once, a section's pieces and the textures it is the first to use sit
// together in it, and LZ4 has it at 43% of the loose bytes, which is what the
// SD card's few MB/s were spending the page-in on.
#define WORLD_ARCHIVE   "WORLD.NFSZ"
#define ARC_MAGIC       0x5a53464e   // 'NFSZ' little-endian
#define ARC_VERSION     1
#define ARC_HDR         0x40
#define ARC_SEC         0x30
#define ARC_TEX         0x10
#define ARC_STORE       0
#define ARC_ZLIB        1
#define ARC_LZ4         2
// The worker's buffer for packed bytes is the biggest packed piece or this,
// whichever is more: textures stored back to back are read this many bytes at
// a time.
#define ARC_RUN_BYTES   (512u * 1024)

// Paging radii, in metres, measured from `pos` to a section's world bounding
// box (zero inside it). An L4RA street section is roughly 300-400 m across, so
// LOAD keeps the ring of neighbours around the car up, and the gap to DROP is
// the hysteresis: without it, standing on a boundary re-reads two sections off
// the SD card every other frame.
#define LOAD_RADIUS  260.0f
#define DROP_RADIUS  420.0f
// 520 was set to match the old fog curve's saturation point and cost most of a
// frame for nothing. Measured on the career spawn (C33): 23 sections resident,
// 702 batches and 126k triangles between them, ~17 of them surviving frustum
// culling -- about 500 draw calls and 90k triangles submitted per frame, with
// C3D_FrameBegin blocking 14 ms waiting on the GPU behind a 9 ms submit. That
// is GPU-bound, and the cheapest triangles to stop drawing are the ones the fog
// has already turned into flat horizon colour.
//
// At the old density the fog was 90% opaque by 420 m and 97% by 520 m, so the
// outer 120 m of the radius contributed almost no visible pixels while costing
// area proportional to r^2. Pulling the edge in to 400 m and moving the fog's
// saturation point in with it (WORLD_FOG_DENSITY, main.c) keeps the *look* --
// sections still reach the edge already the colour of the horizon, which is the
// whole point of having fog -- while cutting drawn area to (400/520)^2 = 59% of
// what it was. DROP_RADIUS now also sits outside DRAW_RADIUS rather than inside
// it, which it should always have: a section between 420 and 520 m used to be
// dropped by the pager and then asked for by the drawer on the same frame.
#define DRAW_RADIUS  400.0f

// Which level of detail a prop is drawn at (TASKS.md #173, #247). The retail
// scenery records name the same model at three levels -- A, B and Z -- and
// tools/track_pack.py bakes a section's A and B models side by side. Which one
// an instance draws is retail's own choice, made per instance every frame from
// how large it is on screen (Speed.elf DrawAScenery 0x80208fa4, in model3d.c's
// instancePick): world_set_lod_view() hands it the camera, and a section
// packed since #247 carries the instance table it needs. Z is the reflection
// views' level, which a player view never draws.
//
// This replaces LOD_SWITCH, the port's own rule: one switch per *section*, at
// 40 m from its nearest corner, with the far side preferring Z. Neither build
// measures to a section: each instance is sized on its own, so a large one far
// off can keep its A while a small one close by has dropped to its B, and one
// small enough on screen is not drawn at all. A pack from before #247 has no
// table to make that choice
// from, so every street tile draws its A models and only the pinned far
// scenery its B -- re-pack the world (tools/pack_world.py) for the real rule.

// Resident budget. A section costs its vertex pool + index pool + swizzled
// textures, which is what the index's `bytes` field measures, and all of it is
// linear (GPU-visible) memory.
//
// This used to be a third of whatever the linear heap turned out to be, with
// the other two thirds left "to the frontend texture packs, the car, and the
// video decoder". That reservation was already obsolete when it was written:
// worldEnter() calls frontCacheRelease() and drops the showroom model before
// it ever gets here, and the video decoder is finished with by the time the
// frontend is reachable at all. So the divisor was reserving two thirds of
// GPU-visible memory for things that are, by construction, not resident while
// driving -- and the world is the only thing on screen.
//
// It is now measured instead: whatever the linear heap has free at the moment
// the world opens, minus a reserve for what still has to be allocated after
// this point. That reserve is named rather than guessed:
//   * the in-game overlays -- UI_pause.fng, UI_InGame_WorldMap.fng and
//     UI_SMS_Mailbox.fng -- each load a frontend texture pack while the world
//     stays resident, and the pause screen is the only way out of the world,
//     so it failing to allocate is not an acceptable outcome. Their art comes
//     from InGameCommon.bun (1.6 MB of DXT on disk) and texpack.c expands it
//     to power-of-two-padded RGBA8, so the worst case if one screen wanted all
//     of it at once is around 13 MB;
//   * SKYDOME.nfsc and SHADOW.nfsc, loaded a few lines after world_open() in
//     worldEnter(), come to 0.3 MB;
//   * and the balance is slack against fragmentation, which matters here in a
//     way it does not for a static allocation: the pager frees and re-allocates
//     multi-hundred-KB section blobs continuously for the whole drive, so a
//     budget that filled the heap exactly would eventually fail an allocation
//     it nominally had room for.
// WORLD_MAX_RESIDENT is the second brake on top of it, and at whole-world
// scale it is the one that actually bites: L4RA's street
// tiles average a third of a megabyte, so twenty of them plus the pinned far
// scenery and the bank still sit at a third of the budget. Twelve was tuned
// against an 18-section corner; at 344 sections it left the ring around the car
// one slot short and the pager swapped a section in and out every frame.
// Sharpening bias applied to every mipmapped bank texture, in mip levels.
// Negative sharpens. See bankAcquire() for why it is not zero.
#define BANK_LOD_BIAS         (-0.5f)
static float s_bankLodBias = BANK_LOD_BIAS;
static int   s_forceTexBase = -1;   // world_force_tex_base()
// Whether the bank uploads its mip chains and lets the GPU choose between
// levels. On, as retail has it: TextureInfoPlatInfo::SetImage (Speed.elf
// 0x801EA488) gives every texture with a chain GXInitTexObjLOD(LIN_MIP_LIN,
// LINEAR, min 0, max levels-1, bias 0, no edge LOD, ANISO_1) -- plain
// trilinear. A texture flagged at +0x55 loses its last level (max - 1).
//
// It was off from #229 to #252, because of what the choice does to a road.
// The PICA200 picks its level off the LARGER of the two UV derivatives and has
// no anisotropic filtering to correct it with. A street is drawn at a grazing
// angle essentially always, so the derivative running away from the camera is
// routinely eight times the one across the lane -- three whole mip levels --
// and the across-the-road detail, still near 1:1 on screen, is minified just
// as hard. The result was not softness but a road with no asphalt and no lane
// markings: a flat beige smear a few metres past the bumper.
//
// Turning the chains off for everything to fix that cost half the card's GPU
// time. #252's parked sweep: in M33 the frame is 33.4 ms of GPU at base level
// and 13.9 with the chains (frame 39.8 -> 20.5 ms); in F31 18.9 -> 11.9. A
// minified texture with no smaller level misses the texture cache on nearly
// every pixel, and the bank is in FCRAM. The dense views are mostly distant
// facades, which is exactly what the chain is for.
//
// So the smear is kept off where it happened and nowhere else: a ground
// texture -- see BankTexture.ground -- samples its base level only, and
// everything else takes the chain. PORT: retail mips the road too; on the
// GameCube's 640x448 a road texel is minified half as hard as on the 3DS's
// 400x240, which is the smear's whole margin.
static bool  s_bankMips = true;
// DbgGpuSweep's "groundmips" step: the ground takes the chain as well, so the
// card can say what keeping the road sharp costs. world_set_ground_mips().
static bool  s_groundMips = false;
// How the uploaded chains are sampled, under DbgGpuSweep (TASKS.md #252):
// world_set_tex_mode(). Read only when s_bankMips is on, since without it there
// is no chain to choose from.
static WorldTexMode s_texMode = WORLD_TEX_CHAIN;
// g_TextureFiltering (#326, world_set_tex_filter): bilinear, or point.
static GPU_TEXTURE_FILTER_PARAM s_texFilter = GPU_LINEAR;
// Whether a section's vertices and indices are read straight into the GPU
// pools by the loader thread (#226) instead of being copied there on the main
// one. On: it is the larger half of what took a section commit from 25.5 ms
// to a 1.7 ms worst frame. It did tear the world's geometry when it first
// went in, but that was the table buffer being built without its header (see
// LOADER_JOB_HDR) rather than anything about reading from the worker, and it
// has run clean since. DbgSplitLoad=0 turns it off.
static bool  s_splitLoad = true;
// world_set_draw_debug(); all off unless a Dbg* config key says otherwise.
static bool s_dbgNoFarScenery = false;
static bool s_dbgNearLod = false;
// Turns the sub-section cull (#68) off, so what a frame costs with it and
// without it can be read off two runs of the same route rather than two
// builds. The cells are still built and the table still costs its memory --
// this is the draw-side test, which is the part that is supposed to be free.
static bool s_dbgNoCull = false;
// world_section_serial()'s counter, and what world_draw() draws the props with.
static u32 s_sectionSerial = 0;
static WorldPropDraw s_propDraw = NULL;
static void* s_propDrawUd = NULL;
// world_instance_lod()'s: the instance states world_draw() last worked out,
// and how many more instances it may look for this frame. A look is a walk of
// the section's instance table, and a section paging in brings dozens of
// marked props at once.
#define WORLD_INST_LOOKS  32
static u32 s_drawLodFrame = 0;
static int s_instLooks = 0;

#define WORLD_LINEAR_RESERVE  (16u * 1024 * 1024)
// And what worldEnter() loads after world_open() and keeps while driving: the
// player's car and sim, the HUD, engage, sky and shadow; in a race the
// opponents' cars instead of the free-roam markers. Measured, not reasoned
// (2026-10-25, Azahar, career save, the `world: mem` lines): 4.4 MB in free
// roam, about the same in a race. The reserve above was written believing
// these were gone before the world opened; they are not, and topology's 7 MB
// tree came out of it too until world_open() sized the budget after it. With
// the world's memory in an arena of its own (linarena.h) a short reserve is
// no longer a fragmented heap but a pause menu that does not open.
#define WORLD_IN_WORLD_RESERVE ( 5u * 1024 * 1024)
// Only reached if linearSpaceFree() answers something that cannot be right
// (it is a pool walk, not a syscall, so in practice it does not). The old
// third-of-the-heap rule is kept for exactly that case.
#define WORLD_BUDGET_DIVISOR 3
#define WORLD_BUDGET_FALLBACK (16u * 1024 * 1024)
#define WORLD_MAX_RESIDENT 20

// How much closer an incoming section has to be than the resident it would
// evict, as a ratio of squared distance (1.44 = 20% in metres). Whenever the
// slot cap is the binding constraint, two sections at nearly equal range will
// otherwise take turns being "nearest missing" and "farthest resident", and
// each swap is an SD read plus a texture upload for no visible change.
#define EVICT_MARGIN 1.44f

// The streaming order (TASKS.md #270 P1): retail's TrackStreamer, decoded in
// docs/retail_trackstreamer.md. With L4RA.BUN's visible lists loaded, what is
// wanted is the list of the drivable section the car is in plus the list of
// the one it will be in 1.5 s from now (GetPredictedZone 0x802059D4), loaded
// in GetLoadingPriority's order (0x80206E74). The constants with an address
// are read out of Speed.elf.
#define STREAM_PREDICT_S     1.5f      // 0x80338594, look-ahead
#define STREAM_PREDICT_CAP   100.0f    // 0x80338598, and never further than this
#define STREAM_PREDICT_TOP   178.811f  // 0x80338590, faster than this: none
#define STREAM_PRIO_AHEAD_S  1.0f      // GetLoadingPriority measures from 1 s ahead
#define STREAM_PRIO_SPEED    60.0f     // 0x8033864C, the full discount's speed
#define STREAM_PRIO_ANG_LO   20.0f     // 0x8033865C
#define STREAM_PRIO_ANG_HI   90.0f     // 0x80338660
#define STREAM_PRIO_ANG_SPAN 70.0f     // 0x80338664
#define STREAM_PRIO_DISCOUNT 0.67f     // 0x80338668
#define STREAM_PRIO_BUCKET   75.0f     // 0x8033866C
#define STREAM_PRIO_CAP      999.0f    // 0x80338648, GetDistanceOutside's cap
// PORT, not retail: what the port reads beyond retail's lists. The GameCube
// streams CMPR textures; the PICA has no DXT, so the bank holds RGBA8/RGB8, 4-8x
// the bytes, off an SD card. A 250 km/h crossing into the next drivable section
// wants 8.7 MB (median) to 31 MB (worst) that the current list does not hold
// (#270 item 1). So the lists of the drivable sections touching the car's, and
// of the one STREAM_AHEAD_S ahead, are read as well -- into memory nothing
// wanted needs, and only up to STREAM_KEEP_PCT of the budget.
#define STREAM_AHEAD_S       3.0f
#define STREAM_AHEAD_CAP     250.0f
#define STREAM_NEIGHBOUR_GAP 4.0f      // m between two outlines that still touch
// Unwanted sections stay resident until a load needs their memory (SwitchZones
// 0x80206A6C deactivates, UnloadLeastRecentlyUsedSection 0x80205068 frees the
// oldest), but only up to this share of the budget: the rest is slack against
// fragmentation on a heap that is freed and refilled for the whole drive. A
// wanted section may use the whole budget.
#define STREAM_KEEP_PCT      90
// The car more than this far outside the drivable section last found -- put
// down somewhere outside every outline -- and that section's lists say nothing
// about where it is. The radius pager stands in until it is inside one again.
#define STREAM_STALE_M       100.0f
// A load that failed for a reason other than its file -- memory, above all,
// which a race's opponent cars and HUD take out of the heap the budget was
// sized from -- is tried again this many frames later rather than written off.
// Half a second; a read error gets STREAM_RETRIES goes before it counts as a
// bad file, a memory error any number (#270).
#define STREAM_RETRY_FRAMES  30
#define STREAM_RETRIES       3

// What a section is to the streamer this frame; see streamTiers().
enum {
	STREAM_NONE = 0,   // nobody wants it: kept while memory allows, oldest out first
	STREAM_PREFETCH,   // read ahead into spare memory; evicted before a wanted one
	STREAM_WANTED,     // on a list that matters now and inside DRAW_RADIUS: never evicted
};

// Sections the index marks as far scenery -- the skyline plate and the
// distant-LOD shells. They are meant to be visible from anywhere in the world,
// which is exactly why paging them like a city tile does not work: their boxes
// cover most of Bayview, so the car is always inside one and they never drop.
// Pinning them instead is both more correct and cheaper than the alternative:
// L4RA's nine of them come to 3 MB between them, against street tiles that
// average a third of a megabyte each and want every one of the 12 slots.
#define SECTION_FAR 1u
// The world animations' models (track_pack.py SECTION_ANIM, TASKS.md #239):
// pinned like far scenery (SECTION_FAR is set too), but never drawn as a
// section -- worldanim.c draws each model through its entity's matrix.
#define SECTION_ANIM 2u

// Who owns a block of the world arena (arena_tag(), linarena.h), so that its
// compaction can move it and repoint the owner: a section's vertex or index
// pool, or a bank texture's chain. The low bits are the index.
#define TAG_SECTION 0x10000000u
#define TAG_TEXTURE 0x20000000u
#define TAG_KIND    0xF0000000u

typedef struct {
	u32     hash;
	u16     w, h, fmt;
	u16     maxLevel;  // mip levels below level 0 in the file; 0 = no chain
	u32     offset;    // absolute offset of the pixels in WORLD.NFSB
	u32     bytes;     // the WHOLE chain, which is what the file holds
	u32     uploaded;  // what actually went to the GPU (see w->texBaseLevel)
	int     refs;      // resident sections currently using this texture
	bool    valid;     // `tex` is initialised and uploaded
	bool    failed;    // upload was tried and did not work; do not retry
	// A ground surface: the bank record's material is real alpha (usage 2)
	// with an opaque blend, the wet-road mask class model3d.c draws without
	// its alpha (#191). The textures L4RA's ROAD parts use are 98.8% this
	// class by triangle count, and away from the roads it is ground art too:
	// courts, concrete, sidewalks, parking, water, boardwalk. Two walls
	// (ARC_FACTORYBRICK_01, ARC_WALLMARINA_A) are the only exceptions, 693 of
	// its 157k triangles. Sampled at its base level only; see s_bankMips.
	bool    ground;
	bool    pending;   // `tex` is allocated and the worker is reading into it
	                   // -- see bankBeginUpload(). Not valid, not drawable,
	                   // and owned by the load in flight, so whatever ends
	                   // that load has to delete it (bankCancelPending).
	u8      arcCodec;  // in WORLD.NFSZ, when the world reads it: how the chain
	u32     arcOff;    // is stored, where, and in how many bytes. It decodes
	u32     arcBytes;  // to exactly `bytes`.
	C3D_Tex tex;
} BankTexture;

// One section's entry in WORLD.NFSZ: `off` 0 = no file when it was packed.
// The pieces are the header and tables, the vertices, the indices and the
// instance table, back to back from `off` (or the whole file, as one piece).
typedef struct {
	u32 off, span, raw;
	u8  codec, pieces;
	u32 comp[4];   // stored bytes
	u32 praw[4];   // decoded bytes
} ArcSection;

// Bytes one mip chain occupies: level 0 plus `maxLevel` levels below it, each
// a quarter of the one above. Written as a loop rather than citro3d's closed
// form because this validates a file against what the GPU will be told to
// allocate, and the two have to agree exactly -- a chain that is one level
// long on disk and two levels long in the texture object is a GPU read past
// the end of the allocation, not a visual glitch.
static u32 chainBytes(u32 level0, u32 maxLevel) {
	u32 total = level0, sz = level0;
	for (u32 i = 0; i < maxLevel; i++) {
		sz >>= 2;
		total += sz;
	}
	return total;
}

// A bank record's format (tools/track_pack.py, tools/etc1_tex.py): 0 RGB565
// and 1 RGBA4444 are 16 bits a texel; 4 ETC1 (4 bits) and 5 ETC1A4 (8 bits)
// are the PICA's block formats, which the world bank is packed in since
// TASKS.md #427. Each ETC level is still exactly a quarter of the one above
// down to the 8x8 floor (32 / 64 bytes), so chainBytes() holds for them too,
// and citro3d sizes their levels the same way (fmtSize in bits).
#define BANK_FMT_RGB565 0
#define BANK_FMT_RGBA4  1
#define BANK_FMT_ETC1   4
#define BANK_FMT_ETC1A4 5

static bool bankFmtKnown(u16 fmt) {
	return fmt == BANK_FMT_RGB565 || fmt == BANK_FMT_RGBA4 ||
	       fmt == BANK_FMT_ETC1 || fmt == BANK_FMT_ETC1A4;
}

static u32 bankLevel0Bytes(u32 w, u32 h, u16 fmt) {
	const u32 texels = w * h;
	return fmt == BANK_FMT_ETC1 ? texels / 2
	     : fmt == BANK_FMT_ETC1A4 ? texels : texels * 2;
}

static GPU_TEXCOLOR bankGpuFmt(u16 fmt) {
	switch (fmt) {
	case BANK_FMT_RGB565: return GPU_RGB565;
	case BANK_FMT_ETC1:   return GPU_ETC1;
	case BANK_FMT_ETC1A4: return GPU_ETC1A4;
	default:              return GPU_RGBA4;
	}
}

// ---------------------------------------------------------------- loader
//
// Paging used to do its reads on the main thread, inside world_update(), which
// meant one frame in every section swap took as long as the read: measured in
// Azahar over a career drive, 33-400 ms for the .nfsc and another 7-170 ms for
// the section's share of the texture bank, worst case 837 ms in a single frame
// on C4 (#176). The stutter the user reported was exactly that.
//
// What the measurement also showed is that none of it is work. Parsing the
// blob, copying it onto the linear heap, and C3D_TexInit/Upload/Flush for
// every texture together come to under 10 ms even on the largest section --
// the read time is the thread sitting blocked in FSFILE_Read. So none of it
// needs to be split into slices or budgeted per frame. It needs to happen on a
// thread that is allowed to block, which is what this is.
//
// The split is: the main thread does every allocation and everything the GPU
// can see, and the worker fills what it allocated. Vertices and indices go
// straight into the GPU pools (#226) and texture pixels straight into the
// textures (#230) -- in both cases into memory the main thread reserved and
// nothing has been told about yet, so the handover is the done event and
// nothing more. The one piece of real work the worker does is the in-flight
// model's cull cells and physics grid, which are plain CPU over memory only
// it holds at that point.
// Nothing is shared except the request block, handed over by the two events,
// and the worker holds its own FILE* for the bank so the two threads never
// touch one stdio stream.
#define LOADER_MAX_TEX  256    // C4, the heaviest section in L4RA, wants 121

// How many of a section's texture slots are bound per frame. This was four
// while a bind was a linearAlloc plus a quarter-megabyte memcpy; since #230 it
// is a C3D_TexFlush and a refcount, so the budget is no longer paying for the
// expensive thing and the cost of a small one is real: C4's 121 textures at
// four a frame left the section undrawable for half a second of driving.
// Sixteen finishes it in eight frames. The load line reports `bind` and the
// worst main-thread frame, so if this is ever too many it says so.
#define BIND_PER_FRAME  16
// The three steps a section is loaded in. The split between HDR and GEO is
// what keeps the main thread out of the section's bulk: HDR reads enough to
// describe the file, the main thread allocates from that, and GEO reads the
// vertices and indices straight into what it allocated (TASKS.md #226).
#define LOADER_JOB_HDR  0
#define LOADER_JOB_GEO  1
#define LOADER_JOB_TEX  2

typedef struct {
	u32   hash;
	u32   offset;  // where the levels that are kept start in WORLD.NFSB
	u32   bytes;   // how many of them there are
	void* dst;     // linear memory inside the texture, from bankBeginUpload()
	bool  read;    // the worker got all of `bytes` into `dst`
	// From WORLD.NFSZ instead: the whole stored chain, which decodes to `raw`
	// bytes, the first `skip` of them levels that are not kept.
	u8    codec;
	u32   arcOff, arcBytes, raw, skip;
} TexFetch;

typedef struct {
	Thread     thread;
	LightEvent wake;      // main -> worker: a request is in the block below
	LightEvent done;      // worker -> main: it is finished
	volatile bool quit;

	// Main-thread only.
	bool   busy;          // a request is in flight; do not touch the block
	int    sec;           // which section it is for
	u64    submitTick;    // for the wall-clock figure in the load log
	u64    mainTicks;     // time this load cost the main thread, in total
	u64    mainWorst;     // and the worst single frame of it, which is the
	                      // one that shows -- see BIND_PER_FRAME

	// Handed across. Written by main before signalling `wake`, read by the
	// worker; written by the worker before signalling `done`, read by main.
	int    job;
	bool   ok;
	int    err;           // LOADER_ERR_*, when `ok` is false
	int    errNo;         // and errno at the time, for the log
	u8*    blob;
	long   blobSize;
	TexFetch tex[LOADER_MAX_TEX];
	int      texCount;

	// Built from `blob` once it arrives and held here, not in the section,
	// until its textures are bound too -- publishing it earlier would give
	// world_draw() one frame of an untextured section. The worker builds its
	// physics grid during the texture job, so nothing else may touch it while
	// that job is in flight.
	Model3D* model;
	size_t   gridBytes;   // what that grid took, for the load log
	u64      gridTicks;   // and how long, on the worker
	size_t   cellBytes;   // and the same two for the cull cells (#68)
	u64      cellTicks;

	// The streamed path. `tables` is the worker's copy of the texture and
	// batch tables, which the main thread builds the model from and then
	// frees, while the geometry's pieces wait for LOADER_JOB_GEO. `streamed`
	// is false for a section that had to be read whole -- see
	// loaderArcHeader().
	Model3DPlan plan;
	u8*         tables;
	bool        streamed;
	u8          hdr[MODEL3D_HDR_BYTES];
	u64         bindTicks;   // of mainTicks, what binding textures cost
	int         boundCount;  // and how many textures it actually uploaded
	int         syncCount;   // of those, how many the main thread read itself
	int         bindIdx;     // next slot to bind, across the frames it takes
	bool        binding;     // a section is part-bound and owes more frames

	// Cleared once the world has topology: the physics grid indexes the drawn
	// triangles for collision scans that no longer happen, and building it was
	// 15.4 ms of a 57.9 ms section commit on hardware (#236).
	bool   physGrid;

	// Where a load's wall time goes (#270): each job's time on the worker and
	// the bytes it read, by LOADER_JOB_*. The texture job's figure leaves out
	// the cull cells and grid, which it reports on its own.
	u64    jobTicks[3];
	u32    jobBytes[3];
	int    texReads;

	// The archive (#270 P2+P3+P7), the world's only source since 2026-10-26.
	// `arc`, this load's entry, is set by submitSection(); the rest live as
	// long as the world.
	ArcSection arc;
	FILE*      arcFile;      // the worker's own handle on WORLD.NFSZ
	u32        arcPos;       // where it is, so reads in order need no seek
	u8*        scratch;      // packed bytes on their way to being decoded
	u32        scratchBytes;
	u64        decodeTicks;  // of jobTicks, the share spent decoding
	u32        rawBytes;     // what jobBytes decoded to

	// Read-ahead (#270 P6). Archive bytes [pfOff, pfOff + pfHave) are in
	// `pfBuf`, read by the worker after it has signalled `done`, while the
	// main thread works on what it handed back: the rest of a section's
	// geometry after its header job, and after a section's last job the
	// header and geometry of `hint`, the one the main thread expects to ask
	// for next. A new request stops it within RA_CHUNK. Worker-only, apart
	// from `hint` and `hintArc` (main, before `wake`) and `reqPending`.
	u8*        pfBuf;
	u32        pfCap, pfOff, pfHave;
	int        hint;
	ArcSection hintArc;
	bool       hinted;         // main: this load is the last one's hint
	volatile bool reqPending;  // set before `wake`, cleared as the worker takes it
	bool       merged;         // the geometry job goes straight on to the textures
	u32        aheadBytes;     // of this load's jobBytes, those read ahead
	u32        raBytes;        // everything read ahead so far, used or not
	u64        raTicks;        // and the time it took
} Loader;

// Why a job came back without `ok` (#270). A file that is not there, or is
// not a section, will not be either next time; the rest are this moment, not
// this file, and abandonLoad() retries them. Every one of the user's "holes
// that never loaded" on 2026-10-25 was the second kind logged as the first:
// eight A sections whose files were on the card, written off for good.
enum {
	LOADER_ERR_NONE = 0,
	LOADER_ERR_MISSING,   // fopen: no such file
	LOADER_ERR_OPEN,      // fopen failed some other way; errNo says how
	LOADER_ERR_BAD,       // too short to hold a header
	LOADER_ERR_READ,      // a read came back short
	LOADER_ERR_HEAP,      // malloc refused the tables or the file
	LOADER_ERR_LINEAR,    // the model's vertex and index pools did not allocate
};

static const char* loaderErrName(int e) {
	switch (e) {
	case LOADER_ERR_MISSING: return "no file";
	case LOADER_ERR_OPEN:    return "open failed";
	case LOADER_ERR_BAD:     return "not a section";
	case LOADER_ERR_READ:    return "read failed";
	case LOADER_ERR_HEAP:    return "out of heap";
	case LOADER_ERR_LINEAR:  return "out of linear memory";
	default:                 return "unknown";
	}
}

// Charge a slice of main-thread work to the load in flight. The total is what
// the load cost; the worst slice is what a frame actually had to absorb, and
// since the binding spread those are no longer the same number.
static void mainCost(Loader* L, u64 ticks) {
	L->mainTicks += ticks;
	if (ticks > L->mainWorst)
		L->mainWorst = ticks;
}

typedef struct {
	char     name[WORLD_NAME_LEN];
	float    bbmin[3];
	float    bbmax[3];
	// The box the section's geometry actually reaches (drawing culls against
	// this), and the trimmed one holding all but the outermost couple of
	// percent of its vertices, which is where its content really is and so
	// what paging measures. For a street tile they are nearly the same box.
	float    coremin[3];
	float    coremax[3];
	u32      flags;
	u32      bytes;
	u32      numVerts;
	u32      numIdx;
	u32      numBatches;
	// What the section's two GPU pools take out of the arena -- what the
	// budget counts for its geometry. `bytes` is the file, whose tables and
	// instance list are malloc and freed after the build.
	u32      memBytes;
	Model3D* model;
	C3D_Tex** texSlots;   // one bank texture per the model's texture slots
	bool     failed;   // load was attempted and did not work; do not retry
	// The loader thread is reading this section right now. It has no model
	// yet, so every model-keyed test still treats it as absent -- this exists
	// so world_update() does not pick it a second time while it is in flight.
	bool     loading;
	// The streamer's view of it (#270 P1): this frame's STREAM_*, and the
	// frame it was last wanted -- retail's section+0x34, the LRU key.
	u8       tier;
	u32      lastWanted;
	// The next part of a section tools/track_pack.py split at its index pool
	// ("C4" -> "C4.1"), -1 for none. The parts are one retail section: they
	// share its number, so they are wanted and drawn with it.
	s16      nextPart;
	// A load that failed for want of memory, or on a read, waits until this
	// frame before it is picked again (0 = not waiting); `retries` counts the
	// failures that were not memory -- see abandonLoad().
	u32      retryFrame;
	u8       retries;
	// Texture slots bound empty because the texture would not allocate: drawn
	// in MISSING_TEXTURE_PAINT until texBackfill() gets them in.
	u16      texMissing;
	// Which load of it `model` is, a new number each time one goes in
	// (world_section_serial, for the smokeable props, #318).
	u32      serial;
} WorldSection;

// One retail drivable section: its outline and the sections visible from it.
typedef struct {
	u16   number;        // retail section number, letter * 100 + n
	u16   numPoints;     // 0 = no outline found, never matched
	u32   firstPoint;    // into World::visPts
	float bbmin[2], bbmax[2];
	u32   firstVisible;  // into World::visList
	u32   numVisible;
	u32   firstNeigh;    // into World::visNeigh: the drivable sections whose
	u32   numNeigh;      // outlines touch this one's (#270 P1)
} VisRegion;

struct World {
	char          dir[320];
	int           count;
	WorldSection* secs;
	float         bbmin[3];
	float         bbmax[3];
	int           resident;
	int           residentNear;   // excludes the pinned far scenery
	u32           residentBytes;
	u32           budget;

	FILE*         bankFile;    // the main thread's handle on WORLD.NFSZ
	BankTexture*  bank;        // sorted by hash
	int           bankCount;
	u32           bankBytes;   // pixels currently uploaded out of the bank
	u32           texBaseLevel; // mip levels skipped on upload (see computeTexBaseLevel)

	// WORLD.NFSZ, built from this WORLD.NFSX (arcOpen): every section's
	// entry, and its texture table until bankOpen() has read it.
	ArcSection*   arcSec;
	u8*           arcTex;
	u32           arcTexCount;

	// Retail section visibility; all NULL/0, with visCurrent -1, when L4RA.BUN
	// was not found, which draws every resident section as before. See
	// world_load_visibility().
	VisRegion*    vis;          // the drivable sections
	int           visCount;
	float*        visPts;       // every outline's points, x/y pairs
	s16*          visList;      // every visible list, as indices into secs
	int           visCurrent;   // the drivable section the car is in, -1 = none yet
	u8*           secVisible;   // per section: on visCurrent's list
	u16*          visNeigh;     // every region's touching regions, into vis

	// The streaming order (#270 P1); see streamTiers().
	int           visPred;      // the drivable section 1.5 s ahead, -1 = none
	int           visAhead;     // and the one STREAM_AHEAD_S ahead, -1 = none
	float         vel[2];       // the car's, as world_update() was given it
	u32           frame;        // world_update() calls: the LRU clock
	bool          priming;      // world_prime(): wanted sections only
	bool          submitted;    // submitSection() ran since this was cleared
	bool          retailPick;   // the last world_update() picked by streamPick()
	bool          streamFull;   // "does not fit" said, until a load commits
	u32           budgetMax;    // the budget world_open() sized; `budget` is
	                            // lowered under it when memory runs out first
	// Compaction (#270): the free run the arena is kept holding -- the
	// biggest street section's two pools, so whichever is wanted next fits
	// without waiting -- and what moving blocks has cost so far.
	u32           runTarget;
	u32           tidyWait;     // frame before which streamTidy() rests
	u32           tidyBytes;    // copied since the run last stood at target
	u32           tidySteps;
	u64           tidyTicks;
	u32           compactBytes; // since world_open()
	u32           compactSteps;

	// Retail's collision world (TASKS.md #236), resident in full and read by
	// every physics query below. NULL for a pack made before #236, or one
	// whose WORLD.NFST would not load, in which case the old scans of the
	// drawn geometry answer instead.
	Topology*     topo;

	// Retail's scenery groups (TASKS.md #237): WORLD.NFSG, and which of its
	// sets are drawn, one bit each. Every section model borrows the table.
	// NULL and all clear with no WORLD.NFSG, which hides every tagged batch.
	Scenery*      scenery;
	u8            sceneryOn[SCENERY_SET_BYTES];

	Loader        ld;

	// Index section by retail number (letter * 100 + n), -1 for none: what
	// world_section_loaded() answers the light database from (#166).
	s16*          byNumber;

	// Holes (#270): a section on the visible list, close to the car, with
	// nothing loaded -- the street the player drives onto before it draws.
	// See holeTrack().
	bool          holeMute;      // world_prime(): entry is all holes by definition
	u64           holeStart;     // when the open one opened, 0 = none open
	int           holeWorst;     // most sections missing at once in it
	float         holeNearest;   // nearest one came, m
	char          holeName[WORLD_NAME_LEN];
	u32           holeCount;     // since world_open()
	u64           holeTicks;
	float         lastPos[3];    // for the speed the hole opened at
	u64           lastTick;
	float         speed;         // m/s, smoothed
};

static void (*s_log)(const char*) = NULL;

void world_set_log(void (*sink)(const char* msg)) {
	s_log = sink;
	topo_set_log(sink);
	scenery_set_log(sink);
}

static void wlog(const char* fmt, ...) {
	if (!s_log)
		return;
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	s_log(b);
}

static u32 rd32(const u8* p, u32 off) {
	return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) | ((u32)p[off + 3] << 24);
}

static float rdf32(const u8* p, u32 off) {
	union { u32 u; float f; } v;
	v.u = rd32(p, off);
	return v.f;
}

// Squared distance from `pos` to a section's bounding box, in the XY plane
// only. Z is ignored on purpose: the sections are city blocks stacked in a
// grid, an overpass and the street under it belong to the same tile, and a
// section's Z extent is dominated by whatever tall shell it happens to carry.
static float distSqToBoxXY(const float lo[3], const float hi[3], const float pos[3]) {
	float dx = 0.0f, dy = 0.0f;
	if (pos[0] < lo[0])      dx = lo[0] - pos[0];
	else if (pos[0] > hi[0]) dx = pos[0] - hi[0];
	if (pos[1] < lo[1])      dy = lo[1] - pos[1];
	else if (pos[1] > hi[1]) dy = pos[1] - hi[1];
	return dx * dx + dy * dy;
}

// What paging measures: the core box, so a section carrying a skyline shell is
// judged on where its city is rather than on how far its shell reaches.
static float distSqToBox(const WorldSection* s, const float pos[3]) {
	return distSqToBoxXY(s->coremin, s->coremax, pos);
}

// What drawing culls against: the true box. A skyline is supposed to be
// visible from the far side of the map -- that is the whole point of it.
static float distSqToDrawBox(const WorldSection* s, const float pos[3]) {
	return distSqToBoxXY(s->bbmin, s->bbmax, pos);
}

static u32 computeBudget(void) {
	// What is free right now, on the heap the sections and their textures will
	// actually come out of. Called from world_open(), i.e. after the frontend
	// has let go of everything it held.
	size_t freeNow = linearSpaceFree();
	const u32 reserve = WORLD_LINEAR_RESERVE + WORLD_IN_WORLD_RESERVE;
	if (freeNow > reserve) {
		u32 b = (u32)(freeNow - reserve);
		// A heap so small that the reserve eats nearly all of it is not worth
		// trusting this path with; fall through to the old rule.
		if (b >= WORLD_BUDGET_FALLBACK)
			return b;
	}
	u32 linear = g_heapInfo.linearGot;
	if (!linear)
		return WORLD_BUDGET_FALLBACK;
	u32 b = linear / WORLD_BUDGET_DIVISOR;
	return b ? b : WORLD_BUDGET_FALLBACK;
}

// The world's linear memory as one block of its own (linarena.h), sized to
// the budget just computed. A heap that will not give one block that big gives
// the largest it has, which leaves at least the reserve outside it; one that
// will not give even half of the budget leaves the world on the shared heap,
// as it always was, and the log says so.
#define ARENA_SLACK (1024u * 1024)
static void worldArenaOpen(World* w) {
	const u32 freeNow = (u32)linearSpaceFree();
	const u32 largest = heap_linear_largest();
	u32 want = w->budget;
	if (want > largest)
		want = largest & ~0xfffu;
	if (want >= w->budget / 2 && arena_create(want)) {
		// The budget stops ARENA_SLACK short of the edge: it counts what
		// each texture holds, and the arena rounds each one up to 0x80.
		wlog("world: arena %u KB (budget %u KB; linear %u KB free, %u KB largest, %u KB "
			"left for the rest), kept with a %u KB run free", want >> 10, w->budget >> 10,
			freeNow >> 10, largest >> 10, (unsigned)(linearSpaceFree() >> 10),
			w->runTarget >> 10);
		w->budget = want - ARENA_SLACK;
	} else {
		wlog("world: NO arena -- linear %u KB free, %u KB largest, budget %u KB; the "
			"world shares the heap", freeNow >> 10, largest >> 10, w->budget >> 10);
	}
}

// ------------------------------------------------------- the texture bank

static bool isPow2u(u32 v) {
	return v && !(v & (v - 1));
}

// Is this record consistent with a file we can actually upload? Both callers
// have to agree: the loader thread reads `bytes` off the SD card on a worker
// thread and bankAcquire() hands the result to the GPU, so a record the queue
// accepts and the upload rejects wastes a read, and the reverse would upload
// a texture whose declared size does not match its chain. One function, called
// from both, is the only way that stays true.
static bool bankRecordSane(const BankTexture* t) {
	return isPow2u(t->w) && isPow2u(t->h) && t->w >= 8 && t->h >= 8 &&
	       t->maxLevel <= 12 && bankFmtKnown(t->fmt) &&
	       t->bytes == chainBytes(bankLevel0Bytes(t->w, t->h, t->fmt), t->maxLevel);
}

// How much of the bank ends up resident at once, as a percentage of the whole
// bank. This is the one number here that had to be measured rather than
// derived: it is a property of how much texture art neighbouring city tiles
// share, which nothing in the file states.
//
// Measured exactly, not extrapolated. The method, for when the world's art
// changes and this has to be redone: for each of L4RA's 357 street sections in
// turn, take that section's centre as a vantage point, select the sections the
// pager would hold there (everything inside LOAD_RADIUS, capped at
// WORLD_MAX_RESIDENT, plus the pinned far scenery), union the texture hashes
// their .nfsc texture tables declare, and add up those hashes' record lengths
// in the bank. Over all 357 vantage points that comes to a median of 22.2 MB
// and a 90th percentile of 27.9 MB against a 109.2 MB bank, i.e. 25.5%. The
// worst four vantage points in the world (P24, P27, P31, P35 -- the dense
// south-east grid) reach 33.8 MB, 31%.
//
// The p90 is the one used, on the reasoning that the pager's own budget check
// already handles an overshoot by evicting a section, which costs one distant
// tile popping rather than anything breaking -- so sizing for the worst tile
// in Bayview would cost image quality across the entire city to avoid a
// recoverable effect in four places.
#define BANK_RESIDENT_PCT 26
// The budget already has WORLD_LINEAR_RESERVE taken out of it for everything
// that is not the world, so this is not a second safety margin -- it is only
// there so the pager is not evicting from the very first frame it draws.
#define BUDGET_HEADROOM_PCT 95

// Which mip level to treat as level 0 when uploading, i.e. how much of the
// packed resolution this console can afford to keep.
//
// This exists because the art and the hardware are no longer decided together.
// track_pack.py --maxdim sets what is IN the file, and the same file now has to
// serve an Old 3DS (whose linear heap is about two thirds of a New 3DS's, see
// heapcfg.c) and a New one. Skipping the top level of every chain quarters the
// resident bank and costs nothing at load time -- the levels are already in the
// file, they are simply not uploaded -- so a 256px pack degrades to exactly the
// 128px world the port used to ship, on the console that cannot hold it, and
// stays 256px on the console that can.
//
// Everything it reasons from is measured on this machine at this moment rather
// than assumed:
//   * `budget` comes from the linear heap the kernel actually granted;
//   * the geometry projection comes from the index's own per-section byte
//     counts, so it is this world's real geometry, not a remembered figure;
//   * the bank projection comes from the bank's own payload size.
// The one estimate is BANK_RESIDENT_PCT above.
//
// Returns 0..3. 3 is a floor because the chain itself stops at 8x8 and
// bankAcquire clamps per texture anyway.
static u32 computeTexBaseLevel(const World* w, u32 payloadBytes) {
	// Projected resident geometry: the pinned far sections are always up, plus
	// WORLD_MAX_RESIDENT average street tiles. Checked against the same
	// vantage-point sweep described above, which knows each section's real
	// neighbours rather than assuming a full slot table: this estimate gives
	// 13.77 MB and the sweep's 90th percentile is 13.66 MB, so the cheap
	// version is within 1% of the exact one and the exact one is not worth
	// 127k box tests at load time.
	u64 farBytes = 0, nearBytes = 0;
	int nearCount = 0;
	for (int i = 0; i < w->count; i++) {
		if (w->secs[i].flags & SECTION_FAR) {
			farBytes += w->secs[i].memBytes;
		} else {
			nearBytes += w->secs[i].memBytes;
			nearCount++;
		}
	}
	u64 geom = farBytes;
	if (nearCount)
		geom += (nearBytes / (u32)nearCount) * WORLD_MAX_RESIDENT;

	u64 allowed = (u64)w->budget * BUDGET_HEADROOM_PCT / 100;
	if (geom >= allowed) {
		// No room for the bank at all at any level. Take the smallest one and
		// let the pager's own budget check throttle residency; dropping mip
		// levels cannot fix a geometry overrun.
		wlog("world: geometry projection %u KB alone exceeds %u KB",
		     (u32)(geom >> 10), (u32)(allowed >> 10));
		return 3;
	}
	u64 forBank = allowed - geom;

	u64 resident = (u64)payloadBytes * BANK_RESIDENT_PCT / 100;
	for (u32 base = 0; base < 3; base++) {
		if (resident <= forBank) {
			wlog("world: tex base level %u (bank ~%u KB of %u KB free)",
			     base, (u32)(resident >> 10), (u32)(forBank >> 10));
			return base;
		}
		resident >>= 2;
	}
	wlog("world: tex base level 3 (bank ~%u KB of %u KB free)",
	     (u32)(resident >> 10), (u32)(forBank >> 10));
	return 3;
}

// ---------------------------------------------------------------- archive

// One stored blob, `comp` bytes at `src`, decoded into `raw` bytes at `dst`.
static bool arcDecode(u8 codec, const u8* src, u32 comp, void* dst, u32 raw) {
	if (!raw)
		return !comp;
	switch (codec) {
	case ARC_STORE:
		if (comp != raw)
			return false;
		memcpy(dst, src, raw);
		return true;
	case ARC_ZLIB: {
		uLongf got = raw;
		return uncompress((Bytef*)dst, &got, src, comp) == Z_OK && got == raw;
	}
	case ARC_LZ4:
		return lz4_decode(src, comp, (u8*)dst, raw);
	}
	return false;
}

// One texture's chain out of WORLD.NFSZ into `dst`, through heap of its own:
// the main thread's synchronous reads, and the worker's for a chain too big
// for its buffer. A packed chain decodes from its start or not at all, so
// `skip` bytes of levels that are not kept are decoded and dropped.
static bool arcReadTexture(FILE* f, u8 codec, u32 off, u32 comp, u32 raw, u32 skip,
                           void* dst, u32 want) {
	if (!f || skip > raw || want > raw - skip)
		return false;
	if (codec == ARC_STORE)
		return comp == raw && fseek(f, (long)(off + skip), SEEK_SET) == 0 &&
		       fread(dst, 1, want, f) == want;
	u8* c = (u8*)malloc(comp ? comp : 1);
	u8* r = (skip || want != raw) ? (u8*)malloc(raw) : (u8*)dst;
	const bool ok = c && r && fseek(f, (long)off, SEEK_SET) == 0 &&
		fread(c, 1, comp, f) == comp && arcDecode(codec, c, comp, r, raw);
	if (ok && r != dst)
		memcpy(dst, r + skip, want);
	free(c);
	if (r != dst)
		free(r);
	return ok;
}

// Why the last world_open() failed, for the caller to put in front of the
// player: since WORLD.NFSZ is required, a card without it, or with one built
// from another WORLD.NFSX, is the likely way to get here.
static char s_openError[160];

const char* world_open_error(void) {
	return s_openError;
}

// Opens WORLD.NFSZ, which has to be there and built from this index -- `hdr`
// and `recs` are WORLD.NFSX's bytes, whose CRC the archive carries -- and
// reads its section table, keeping the texture table for bankOpen(). The
// main thread's handle is left in front of the archive's copy of WORLD.NFSB's
// header and records, which bankOpen() reads as it would the bank's own, and
// the worker gets a handle and a buffer of its own. False, with
// s_openError saying why, when anything is wrong with it: the world does not
// open without it.
static bool arcOpen(World* w, const u8* hdr, const u8* recs) {
	Loader* L = &w->ld;
	char path[400];
	snprintf(path, sizeof(path), "%s/%s", w->dir, WORLD_ARCHIVE);
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(s_openError, sizeof(s_openError), "%s is missing", WORLD_ARCHIVE);
		wlog("world: no %s -- it is required", WORLD_ARCHIVE);
		return false;
	}
	const u32 n = (u32)w->count;
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, hdr, WORLD_HDR);
	crc = crc32(crc, recs, n * WORLD_REC);
	const char* why = NULL;
	u8 a[ARC_HDR];
	u32 nTex = 0;
	if (fread(a, 1, ARC_HDR, f) != ARC_HDR || rd32(a, 0) != ARC_MAGIC)
		why = "is not an archive";
	else if (rd32(a, 4) != ARC_VERSION)
		why = "is another version";
	else if (rd32(a, 8) != n || rd32(a, 0x10) != (u32)crc)
		why = "was built from another WORLD.NFSX";
	else {
		nTex = rd32(a, 0x0c);
		// Past the records, the texture-animation table (#326) when the bank
		// has one; bankOpen() reads its length from the bank's header.
		if (!nTex || nTex > BANK_MAX_TEX || rd32(a, 0x20) < BANK_HDR + nTex * BANK_REC ||
		    rd32(a, 0x20) > BANK_HDR + nTex * BANK_REC + BANK_ANIM_MAX_BYTES)
			why = "has a bad texture table";
	}
	u8* st = NULL;
	u8* tex = NULL;
	ArcSection* secs = NULL;
	if (!why) {
		st   = (u8*)malloc((size_t)n * ARC_SEC);
		tex  = (u8*)malloc((size_t)nTex * ARC_TEX);
		secs = (ArcSection*)calloc(n, sizeof(ArcSection));
		if (!st || !tex || !secs)
			why = "does not fit the heap";
		else if (fseek(f, (long)rd32(a, 0x14), SEEK_SET) != 0 ||
		         fread(st, 1, (size_t)n * ARC_SEC, f) != (size_t)n * ARC_SEC ||
		         fseek(f, (long)rd32(a, 0x18), SEEK_SET) != 0 ||
		         fread(tex, 1, (size_t)nTex * ARC_TEX, f) != (size_t)nTex * ARC_TEX ||
		         fseek(f, (long)rd32(a, 0x1c), SEEK_SET) != 0)
			why = "is cut short";
	}
	// The worker's buffer holds the biggest packed piece it reads through it:
	// any texture, and the sections' pieces that are not stored. The
	// read-ahead buffer holds the biggest section's pieces together.
	u32 most = ARC_RUN_BYTES, ahead = 0;
	for (u32 i = 0; i < n && !why; i++) {
		const u8* e = st + (size_t)i * ARC_SEC;
		ArcSection* s = &secs[i];
		s->off    = rd32(e, 0);
		s->span   = rd32(e, 4);
		s->raw    = rd32(e, 8);
		s->codec  = e[0x0c];
		s->pieces = e[0x0d];
		u32 comp = 0, raw = 0;
		for (int k = 0; k < 4; k++) {
			s->comp[k] = rd32(e, 0x10 + k * 4);
			s->praw[k] = rd32(e, 0x20 + k * 4);
			comp += s->comp[k];
			raw  += s->praw[k];
			if (s->codec != ARC_STORE && s->comp[k] > most)
				most = s->comp[k];
		}
		if (s->off && (s->codec > ARC_LZ4 || (s->pieces != 1 && s->pieces != 4) ||
		               comp > s->span || raw > s->raw ||
		               (s->pieces == 1 && s->praw[0] != s->raw)))
			why = "has a bad section entry";
		if (s->off && comp > ahead)
			ahead = comp;
	}
	for (u32 i = 0; i < nTex && !why; i++) {
		const u8* e = tex + (size_t)i * ARC_TEX;
		if (e[0x0c] > ARC_LZ4)
			why = "has a bad texture entry";
		else if (rd32(e, 4) > most)
			most = rd32(e, 4);
	}
	if (!why) {
		L->scratchBytes = most;
		L->scratch = (u8*)malloc(most);
		L->arcFile = fopen(path, "rb");
		if (!L->scratch || !L->arcFile)
			why = "left no room for the loader's buffer";
	}
	free(st);
	if (why) {
		snprintf(s_openError, sizeof(s_openError), "%s %s", WORLD_ARCHIVE, why);
		wlog("world: %s %s -- it is required", WORLD_ARCHIVE, why);
		free(tex);
		free(secs);
		free(L->scratch);
		L->scratch = NULL;
		L->scratchBytes = 0;
		if (L->arcFile)
			fclose(L->arcFile);
		L->arcFile = NULL;
		fclose(f);
		return false;
	}
	L->arcPos = ~0u;
	w->arcSec = secs;
	w->arcTex = tex;
	w->arcTexCount = nTex;
	w->bankFile = f;
	// Without it the loads are what they were before P6, one read at a time.
	L->pfBuf  = ahead ? (u8*)malloc(ahead) : NULL;
	L->pfCap  = L->pfBuf ? ahead : 0;
	L->pfOff  = 0;
	L->pfHave = 0;
	static const char* const codecName[] = { "stored", "zlib", "LZ4" };
	const u32 c = rd32(a, 0x24);
	wlog("world: reading %s -- %u sections, %u textures, %s, loader buffer %u KB, "
	     "read-ahead %u KB", WORLD_ARCHIVE, n, nTex, c <= ARC_LZ4 ? codecName[c] : "?",
	     most >> 10, L->pfCap >> 10);
	return true;
}

static void arcClose(World* w) {
	free(w->arcSec);
	w->arcSec = NULL;
	free(w->arcTex);
	w->arcTex = NULL;
	w->arcTexCount = 0;
}

// Reads the archive's copy of WORLD.NFSB's header and record table, from the
// handle arcOpen() left in front of it; the pixels stay on the SD card and are
// pulled in per texture as sections ask for them, each from the place the
// archive's texture table gives it, in the same order. False, with
// s_openError saying why, when the copy does not read: a world with no bank
// would draw the whole city untextured.
static bool bankOpen(World* w) {
	FILE* f = w->bankFile;
	w->bankFile = NULL;
	const char* why = NULL;
	u8 hdr[BANK_HDR];
	u8* recs = NULL;
	u32 n = 0;
	if (!f)
		why = "has no texture bank";
	else if (fread(hdr, 1, BANK_HDR, f) != BANK_HDR ||
	         rd32(hdr, 0) != BANK_MAGIC || rd32(hdr, 4) < BANK_VERSION_MIN ||
	         rd32(hdr, 4) > BANK_VERSION || rd32(hdr, 0x0c) != BANK_REC)
		why = "holds no v2-3 texture bank";
	else if (n = rd32(hdr, 8), !n || n > BANK_MAX_TEX || n != w->arcTexCount)
		why = "has a bank of another size than its texture table";
	else if (!(recs = (u8*)malloc((size_t)n * BANK_REC)) ||
	         !(w->bank = (BankTexture*)calloc(n, sizeof(BankTexture))))
		why = "has a bank that does not fit the heap";
	else if (fread(recs, 1, (size_t)n * BANK_REC, f) != (size_t)n * BANK_REC)
		why = "is cut short in its bank";
	u8* anim = NULL;
	const u32 animBytes = why ? 0 : rd32(hdr, 0x20);
	if (animBytes > BANK_ANIM_MAX_BYTES)
		why = "has a bad texture-animation table";
	else if (animBytes && (!(anim = (u8*)malloc(animBytes)) ||
	                       fread(anim, 1, animBytes, f) != animBytes))
		why = "is cut short in its texture animations";
	if (why) {
		snprintf(s_openError, sizeof(s_openError), "%s %s", WORLD_ARCHIVE, why);
		wlog("world: %s %s", WORLD_ARCHIVE, why);
		free(recs);
		free(anim);
		free(w->bank);
		w->bank = NULL;
		if (f)
			fclose(f);
		return false;
	}
	// The texture animations (#326): {u32 count, then per one u32 base hash,
	// u16 fps, u8 n, u8 timeBase, u32 frame[n]}.
	model3d_anim_clear();
	if (anim && animBytes >= 4) {
		u32 o = 4;
		for (u32 k = rd32(anim, 0); k > 0 && o + 8 <= animBytes; k--) {
			const int fps = anim[o + 4] | (anim[o + 5] << 8), nf = anim[o + 6];
			if (o + 8 + (u32)nf * 4 > animBytes)
				break;
			u32 frames[32];
			for (int j = 0; j < nf && j < 32; j++)
				frames[j] = rd32(anim, o + 8 + (u32)j * 4);
			model3d_anim_add(frames, nf, fps);
			o += 8 + (u32)nf * 4;
		}
	}
	free(anim);
	const u32 payloadOff = rd32(hdr, 0x10);
	// The scrolling textures (TASKS.md #269): the material's bits 8-9 are the
	// scrollType and the record's last ten bytes its numbers
	// (tools/track_pack.py). Zero there in a bank packed before them.
	model3d_scroll_clear();
	for (u32 i = 0; i < n; i++) {
		const u8* r = recs + (size_t)i * BANK_REC;
		BankTexture* t = &w->bank[i];
		t->hash     = rd32(r, 0);
		t->w        = (u16)(r[4] | (r[5] << 8));
		t->h        = (u16)(r[6] | (r[7] << 8));
		t->fmt      = (u16)(r[8] | (r[9] << 8));
		t->offset   = payloadOff + rd32(r, 0x0c);
		t->bytes    = rd32(r, 0x10);
		t->maxLevel = (u16)(r[0x14] | (r[0x15] << 8));
		const u16 mat = (u16)(r[0x0a] | (r[0x0b] << 8));
		t->ground   = ((mat >> 2) & 3) == 2 && (mat & 3) == 0;
		if ((mat >> 8) & 3)
			model3d_scroll_add(t->hash, (mat >> 8) & 3,
				(s16)(r[0x16] | (r[0x17] << 8)), (s16)(r[0x18] | (r[0x19] << 8)),
				(s16)(r[0x1a] | (r[0x1b] << 8)), (s16)(r[0x1c] | (r[0x1d] << 8)),
				(s16)(r[0x1e] | (r[0x1f] << 8)));
		const u8* e = w->arcTex + (size_t)i * ARC_TEX;
		t->arcOff   = rd32(e, 0);
		t->arcBytes = rd32(e, 4);
		t->arcCodec = e[0x0c];
		if (rd32(e, 8) != t->bytes)
			t->failed = true;   // would not decode to the chain it names
	}
	free(recs);
	free(w->arcTex);
	w->arcTex = NULL;
	w->bankFile  = f;
	w->bankCount = (int)n;
	w->texBaseLevel = computeTexBaseLevel(w, rd32(hdr, 0x14));
	if (s_forceTexBase >= 0 && s_forceTexBase <= 3) {
		w->texBaseLevel = (u32)s_forceTexBase;
		wlog("world: tex base level %d forced (DbgTexBase)", s_forceTexBase);
	}
	int ground = 0;
	for (int i = 0; i < w->bankCount; i++)
		ground += w->bank[i].ground;
	wlog("world: texture bank %d entries (%d ground, %d scrolling, %d animations), %u KB payload, "
	     "mips %s, from %s", w->bankCount, ground, model3d_scroll_count(), model3d_anim_count(),
	     rd32(hdr, 0x14) >> 10,
	     !s_bankMips ? "off" : s_groundMips ? "on" : "on, ground at base", WORLD_ARCHIVE);
	return true;
}

static void bankClose(World* w) {
	if (w->bank) {
		// `pending` as well as `valid`: a track unloaded with a load in
		// flight has textures that are allocated and not yet filled.
		for (int i = 0; i < w->bankCount; i++)
			if (w->bank[i].valid || w->bank[i].pending)
				C3D_TexDelete(&w->bank[i].tex);
		free(w->bank);
		w->bank = NULL;
	}
	if (w->bankFile) {
		fclose(w->bankFile);
		w->bankFile = NULL;
	}
	w->bankCount = 0;
	w->bankBytes = 0;
	model3d_scroll_clear();
	model3d_anim_clear();
}

// The records are written sorted by hash, so this is a binary search.
static BankTexture* bankFind(World* w, u32 hash) {
	int lo = 0, hi = w->bankCount - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		u32 h = w->bank[mid].hash;
		if (h == hash) return &w->bank[mid];
		if (h < hash)  lo = mid + 1;
		else           hi = mid - 1;
	}
	return NULL;
}

// Which level of a record's chain gets treated as level 0, clamped to what
// the chain actually has.
static u32 bankBaseLevel(const World* w, const BankTexture* t) {
	const u32 base = w->texBaseLevel;
	return base > t->maxLevel ? t->maxLevel : base;
}

// What one record actually uploads: the size it ends up on the GPU, how many
// levels go under it, and where in the bank file the first byte of that lands.
// Both upload paths work from this so they cannot disagree about it.
//
// The levels above `base` are skipped. They sit in front of it in the file, so
// the read starts past them -- on an Old 3DS, where `base` is 1, that is four
// fifths of every chain never read at all.
typedef struct {
	u16          w, h;
	u8           maxLevel;
	GPU_TEXCOLOR fmt;
	u32          fileOff;
	u32          bytes;
	u32          head;   // bytes of the chain in front of what is kept
} BankUpload;

static void bankUploadPlan(const World* w, const BankTexture* t,
                           BankUpload* u) {
	const u32 base = bankBaseLevel(w, t);
	const u32 lvl0 = bankLevel0Bytes(t->w, t->h, t->fmt);
	u32 head = 0;
	for (u32 i = 0; i < base; i++)
		head += lvl0 >> (2 * i);
	u->w        = (u16)(t->w >> base);
	u->h        = (u16)(t->h >> base);
	u->maxLevel = s_bankMips ? (u8)(t->maxLevel - base) : 0;   // see s_bankMips
	u->fmt      = bankGpuFmt(t->fmt);
	u->fileOff  = t->offset + head;
	u->bytes    = chainBytes(lvl0 >> (2 * base), u->maxLevel);
	u->head     = head;
}

// Which of an uploaded chain's levels the GPU may pick, per s_texMode. The
// texture's own LOD fields, not its allocation: the levels stay where they are
// and only the range the sampler is allowed into changes, so the sweep can move
// between modes from one frame to the next.
static void bankApplyTexMode(const World* w, BankTexture* t) {
	BankUpload u;
	bankUploadPlan(w, t, &u);
	if (!u.maxLevel)
		return;
	u8 lo = 0, hi = u.maxLevel;
	switch (s_texMode) {
	case WORLD_TEX_BASE:     hi = 0; break;
	case WORLD_TEX_CHAIN:    break;
	case WORLD_TEX_TO_1:     if (hi > 1) hi = 1; break;
	case WORLD_TEX_TO_2:     if (hi > 2) hi = 2; break;
	case WORLD_TEX_SMALLEST: lo = hi; break;
	}
	if (t->ground && !s_groundMips)
		lo = hi = 0;
	t->tex.minLevel = lo;
	t->tex.maxLevel = hi;
	C3D_TexSetLodBias(&t->tex, s_bankLodBias);
}

// #352: the bank's textures in VRAM. The PICA200 reads a texture out of VRAM
// on its own bus; out of FCRAM (the linear heap) it shares the bus with the
// CPUs, and the New 3DS tour put texture reads at ~5 ms of the frame (its
// "texmin" step). VRAM is 6 MB, most of it the render targets', so the bank
// gets what is left: the ground's textures first (the road and the pavement
// under most of the screen, sampled at their base level), then the rest while
// VRAM_GROUND_SLACK is still spare. VRAM_RESERVE stays free for the magazine
// grab's target (main.c magCaptureAlloc, 512x128 RGBA8 + depth). A texture
// moves with the transfer engine's raw TextureCopy (C3D_SyncTextureCopy, as
// citro3d's own C3D_TexLoadImage into VRAM): the CPU may not touch VRAM on a
// real 3DS -- a memcpy into it is a write permission fault (the 2026-11-09
// hardware crash on entering the world), which Azahar does not model. The
// pixels and the C3D_Tex are
// otherwise the same, and C3D_TexDelete() frees either kind (citro3d checks
// the address). Moves happen only between frames (world_vram_tick(), called
// before C3D_FrameBegin()): there C3D_SyncTextureCopy() first waits for the
// GPU's queue to run dry, so nothing queued reads the old copy, and returns
// once the copy is done, so the old copy can be freed.
#define VRAM_RESERVE      (640u * 1024)
#define VRAM_GROUND_SLACK (512u * 1024)
#define VRAM_TICK_BYTES   (256u * 1024)   // a frame's moves outside a mode change
static int s_vramTex = 2;       // 0 none, 1 the ground's, 2 the ground's then the rest
static int s_vramApplied = -1;  // what the bank was last sorted for
static u32 s_vramTicks;
static struct {
	u32 moved, movedBytes, back, failed;
	u64 copyTicks;
} s_vram;

static bool addrInVram(const void* p) {
	const u32 a = (u32)p;
	return a >= 0x1F000000u && a < 0x1F600000u;
}

static bool bankVramWanted(const BankTexture* t) {
	return s_vramTex >= 2 || (s_vramTex == 1 && t->ground);
}

// One move between FCRAM and VRAM, by the GPU's DMA: the linear side's cache
// lines written out and dropped first (the source's pixels reach memory, and
// no dirty line of a fresh destination lands on the copy later), then a raw
// TextureCopy that returns once the transfer is done.
static void vramDmaCopy(const void* from, void* to, u32 bytes, void* linearSide) {
	GSPGPU_FlushDataCache(linearSide, bytes);
	// #481: the copy submits the draws so far itself, unflushed; on the 3DS
	// a running queue then reads the list before the cache has written it.
	C3D_FrameSplit(GX_CMDLIST_FLUSH);
	C3D_SyncTextureCopy((u32*)from, 0, (u32*)to, 0, bytes, GX_TRANSFER_RAW_COPY(1));
}

// Into VRAM. `bytes` is the whole allocation (the chain the upload made).
static bool bankToVram(BankTexture* t, u32 bytes) {
	void* old = t->tex.data;
	if (!bytes || !old || addrInVram(old))
		return false;
	const u32 keep = VRAM_RESERVE + (t->ground ? 0 : VRAM_GROUND_SLACK);
	if (vramSpaceFree() < bytes + keep)
		return false;
	void* v = vramAlloc(bytes);
	if (!v) {
		s_vram.failed++;
		return false;
	}
	const u64 t0 = svcGetSystemTick();
	vramDmaCopy(old, v, bytes, old);
	s_vram.copyTicks += svcGetSystemTick() - t0;
	t->tex.data = v;
	linearFree(old);
	s_vram.moved++;
	s_vram.movedBytes += bytes;
	return true;
}

// Back to the linear heap, where the upload would have put it.
static bool bankToLinear(World* w, BankTexture* t, u32 bytes) {
	void* old = t->tex.data;
	if (!bytes || !old || !addrInVram(old))
		return false;
	const ArenaSide was = arena_route(ARENA_LOW);
	const u32 tag = arena_tag(TAG_TEXTURE | (u32)(t - w->bank));
	void* l = linearAlloc(bytes);
	arena_tag(tag);
	arena_route(was);
	if (!l)
		return false;
	vramDmaCopy(old, l, bytes, l);
	t->tex.data = l;
	vramFree(old);
	s_vram.back++;
	return true;
}

void world_set_vram_tex(int mode) {
	s_vramTex = mode < 0 ? 0 : mode > 2 ? 2 : mode;
}

static void vramReport(const World* w, const char* why) {
	u32 n = 0, bytes = 0, gn = 0, gBytes = 0, gLinear = 0;
	for (int i = 0; i < w->bankCount; i++) {
		const BankTexture* t = &w->bank[i];
		if (!t->valid)
			continue;
		if (addrInVram(t->tex.data)) {
			n++;
			bytes += t->uploaded;
			if (t->ground) {
				gn++;
				gBytes += t->uploaded;
			}
		} else if (t->ground)
			gLinear++;
	}
	const double ms = (double)s_vram.copyTicks * 1000.0 / SYSCLOCK_ARM11;
	wlog("world vram (%s): mode %d -- %u textures in VRAM, %u KB (ground %u, %u KB; %u ground "
		"left in linear); %lu KB VRAM free; moved %u (%u KB, %.1f ms, %.0f MB/s), back %u, "
		"failed %u", why, s_vramTex, (unsigned)n, (unsigned)(bytes >> 10), (unsigned)gn,
		(unsigned)(gBytes >> 10), (unsigned)gLinear, (unsigned long)(vramSpaceFree() >> 10),
		(unsigned)s_vram.moved, (unsigned)(s_vram.movedBytes >> 10), ms,
		ms > 0.0 ? (double)s_vram.movedBytes / 1048576.0 / (ms / 1000.0) : 0.0,
		(unsigned)s_vram.back, (unsigned)s_vram.failed);
}

void world_vram_tick(World* w) {
	if (!w || !w->bankCount)
		return;
	const bool changed = s_vramApplied != s_vramTex;
	s_vramTicks++;
	if (!changed && (s_vramTicks & 15))
		return;
	if (changed) {
		for (int i = 0; i < w->bankCount; i++) {
			BankTexture* t = &w->bank[i];
			if (t->valid && !t->pending && addrInVram(t->tex.data) && !bankVramWanted(t))
				bankToLinear(w, t, t->uploaded);
		}
	}
	u32 budget = changed ? 0xFFFFFFFFu : VRAM_TICK_BYTES;
	for (int pass = 0; pass < 2 && budget; pass++) {
		for (int i = 0; i < w->bankCount; i++) {
			BankTexture* t = &w->bank[i];
			if (!t->valid || t->pending || t->ground != (pass == 0) ||
					addrInVram(t->tex.data) || !bankVramWanted(t) || t->uploaded > budget)
				continue;
			if (bankToVram(t, t->uploaded))
				budget -= t->uploaded;
		}
	}
	s_vramApplied = s_vramTex;
	if (changed || !(s_vramTicks & 4095))
		vramReport(w, changed ? "sorted" : "now");
}

// Allocate the texture and tell the worker where its pixels have to land.
//
// This is the half of an upload that has to stay on the main thread:
// C3D_TexInitWithParams() is a linearAlloc(), and libctru 2.4.1's linear
// allocator is not thread-safe. The other half -- getting a quarter of a
// megabyte of pixels into that allocation -- does not have to, and it is the
// half that costs. On hardware, committing C29 spent 43.9 ms of main-thread
// time binding 102 textures, which was a 14.8 ms worst frame even spread four
// slots to a frame (TASKS.md #230).
//
// The memcpy does not move to the worker; it stops happening. citro3d lays a
// mip chain out contiguously and in the same order the bank file holds it
// (C3D_TexGetImagePtr is data + C3D_TexCalcTotalSize of the levels above it,
// and C3D_TexCalcTotalSize agrees with chainBytes() level for level), so one
// read of the kept chain straight into tex.data puts every level exactly where
// the GPU will look for it. The staging buffer the worker used to malloc, fill
// and have the main thread copy out of is simply gone -- along with 13 MB of
// heap churn per section commit.
//
// Until the read lands the record is `pending`: allocated, not valid, not
// drawable, and owned by the load in flight.
static bool bankBeginUpload(World* w, BankTexture* t, TexFetch* q) {
	BankUpload u;
	bankUploadPlan(w, t, &u);
	C3D_TexInitParams p = { u.w, u.h, u.maxLevel, u.fmt, GPU_TEX_2D, false };
	const ArenaSide was = arena_route(ARENA_LOW);
	const u32 tag = arena_tag(TAG_TEXTURE | (u32)(t - w->bank));
	const bool ok = C3D_TexInitWithParams(&t->tex, NULL, p);
	arena_tag(tag);
	arena_route(was);
	if (!ok)
		return false;   // VRAM/linear pressure; the bind-time path retries
	t->pending  = true;
	q->offset   = u.fileOff;
	q->bytes    = u.bytes;
	q->dst      = C3D_Tex2DGetImagePtr(&t->tex, 0, NULL);
	q->read     = false;
	q->codec    = t->arcCodec;
	q->arcOff   = t->arcOff;
	q->arcBytes = t->arcBytes;
	q->raw      = t->bytes;
	q->skip     = u.head;
	return true;
}

// The GPU side of an upload whose pixels are already in place: make them
// visible to the GPU and publish the record. Microseconds, whichever thread
// read them.
static C3D_Tex* bankFinishUpload(World* w, BankTexture* t) {
	BankUpload u;
	bankUploadPlan(w, t, &u);
	t->pending = false;
	// #352: flushed for the GPU where it is. An upload can finish inside a
	// frame, where a VRAM move cannot wait for its copy (vramDmaCopy());
	// world_vram_tick() moves it before a later frame.
	C3D_TexFlush(&t->tex);
	C3D_TexSetFilter(&t->tex, s_texFilter, s_texFilter);
	// Trilinear across the chain, as retail; bankApplyTexMode() says which of
	// its levels this texture may use.
	if (u.maxLevel) {
		C3D_TexSetFilterMipmap(&t->tex, GPU_LINEAR);
		bankApplyTexMode(w, t);
	}
	// Same reasoning as model3d.c: the retail UVs tile, and V was negated at
	// pack time, which only equals 1-v under repeat.
	C3D_TexSetWrap(&t->tex, GPU_REPEAT, GPU_REPEAT);
	t->valid    = true;
	t->refs     = 1;
	t->uploaded = u.bytes;
	w->bankBytes += t->uploaded;
	w->ld.boundCount++;
	return &t->tex;
}

// The texture the worker never got to, read here the way this always did: one
// the queue could not allocate, or one whose read did not complete. It costs a
// frame on the console -- a 256px chain is around 20 ms off the card -- which
// is why the load line counts these separately.
static C3D_Tex* bankAcquireSync(World* w, BankTexture* t) {
	BankUpload u;
	bankUploadPlan(w, t, &u);
	C3D_TexInitParams p = { u.w, u.h, u.maxLevel, u.fmt, GPU_TEX_2D, false };
	const ArenaSide was = arena_route(ARENA_LOW);
	const u32 tag = arena_tag(TAG_TEXTURE | (u32)(t - w->bank));
	const bool ok = C3D_TexInitWithParams(&t->tex, NULL, p);
	arena_tag(tag);
	arena_route(was);
	if (!ok)
		return NULL;   // VRAM/linear pressure, worth retrying
	w->ld.syncCount++;
	void* dst = C3D_Tex2DGetImagePtr(&t->tex, 0, NULL);
	const bool got = arcReadTexture(w->bankFile, t->arcCodec, t->arcOff, t->arcBytes,
	                                t->bytes, u.head, dst, u.bytes);
	if (!got) {
		C3D_TexDelete(&t->tex);
		t->failed = true;
		return NULL;
	}
	return bankFinishUpload(w, t);
}

// `q`, when given, is the fetch the worker was handed for this texture.
static C3D_Tex* bankAcquire(World* w, u32 hash, const TexFetch* q) {
	if (!w->bankCount || !hash)
		return NULL;
	BankTexture* t = bankFind(w, hash);
	if (!t || t->failed)
		return NULL;
	if (t->valid) {
		t->refs++;
		return &t->tex;
	}
	if (t->pending) {
		if (q && q->read)
			return bankFinishUpload(w, t);
		// The allocation is here but the pixels never arrived. Hand it back
		// and let the synchronous path have another go at both.
		C3D_TexDelete(&t->tex);
		t->pending = false;
	}
	// `bytes` covers the whole mip chain, so the size check has to as well.
	if (!bankRecordSane(t)) {
		t->failed = true;
		return NULL;
	}
	return bankAcquireSync(w, t);
}

static void bankRelease(World* w, C3D_Tex* tex) {
	if (!tex)
		return;
	// The C3D_Tex is a member of its own BankTexture, so the record is just
	// the pointer less the member offset. This used to scan the bank for it,
	// which put 1591 records between every one of a dropped tile's hundred
	// slots and the linearFree() it was after -- on the main thread, in the
	// same frame as the load that evicted it.
	BankTexture* t =
		(BankTexture*)((u8*)tex - offsetof(BankTexture, tex));
	if (t < w->bank || t >= w->bank + w->bankCount || &t->tex != tex)
		return;
	if (--t->refs > 0)
		return;
	C3D_TexDelete(&t->tex);
	t->valid = false;
	t->refs  = 0;
	w->bankBytes -= t->uploaded;
	t->uploaded = 0;
}

// Give back the allocations bankBeginUpload() made for a load that is not
// going to finish. Nothing points at them and their pixels were never
// flushed, so this is the whole of the cleanup -- but it has to happen, or an
// abandoned section leaks its share of the bank until the track is unloaded.
static void bankCancelPending(World* w) {
	Loader* L = &w->ld;
	for (int i = 0; i < L->texCount; i++) {
		BankTexture* t = bankFind(w, L->tex[i].hash);
		if (t && t->pending && !t->valid) {
			C3D_TexDelete(&t->tex);
			t->pending = false;
		}
		L->tex[i].dst  = NULL;
		L->tex[i].read = false;
	}
	L->texCount = 0;
}

// The worker's reads from WORLD.NFSZ (#270 P2+P3+P7). A load walks forward
// through its section's span -- the pieces, then the textures stored behind
// them -- so `arcPos` usually says the handle is already where the next read
// starts, and the seek is skipped.
static bool arcSeek(Loader* L, u32 off) {
	if (L->arcPos == off)
		return true;
	if (fseek(L->arcFile, (long)off, SEEK_SET) != 0) {
		L->arcPos = ~0u;
		return false;
	}
	L->arcPos = off;
	return true;
}

static bool arcRead(Loader* L, void* dst, u32 n) {
	if (fread(dst, 1, n, L->arcFile) != n) {
		L->arcPos = ~0u;
		return false;
	}
	L->arcPos += n;
	L->jobBytes[L->job] += n;
	return true;
}

// Archive bytes [off, off + n) out of the read-ahead buffer (#270 P6), or
// NULL when they are not in it. A piece that starts inside what was read ahead
// and runs past it -- the read-ahead stopped for this job -- has the rest read
// on into the buffer, so nothing is read twice. Of the bytes, those that were
// already there count as this load's here; arcRead() counts the rest.
static const u8* arcAhead(Loader* L, u32 off, u32 n) {
	if (!L->pfBuf || off < L->pfOff || off - L->pfOff > L->pfHave ||
	    n > L->pfCap - (off - L->pfOff))
		return NULL;
	const u32 end = L->pfOff + L->pfHave;
	const u32 had = off + n <= end ? n : end - off;
	if (had < n) {
		if (!arcSeek(L, end) || !arcRead(L, L->pfBuf + L->pfHave, n - had))
			return NULL;
		L->pfHave += n - had;
	}
	L->jobBytes[L->job] += had;
	L->aheadBytes += had;
	return L->pfBuf + (off - L->pfOff);
}

// One piece of the section in flight, `comp` bytes at `off`, decoded into
// `raw` bytes at `dst`: out of the read-ahead buffer when it is there, else a
// stored piece straight there and a packed one through the scratch buffer.
// LOADER_ERR_*. A piece that does not decode is called a read error, not a bad
// file -- a card read that came back wrong is the likely cause, and that one
// is retried.
static int arcReadPiece(Loader* L, u32 off, u8 codec, u32 comp, void* dst, u32 raw) {
	if (!raw)
		return comp ? LOADER_ERR_BAD : LOADER_ERR_NONE;
	const bool stored = codec == ARC_STORE;
	if (stored && comp != raw)
		return LOADER_ERR_BAD;
	const u8* src = arcAhead(L, off, comp);
	if (src && stored) {
		memcpy(dst, src, raw);
	} else if (!src) {
		if (!stored && comp > L->scratchBytes)
			return LOADER_ERR_BAD;
		if (!arcSeek(L, off) || !arcRead(L, stored ? dst : L->scratch, comp))
			return LOADER_ERR_READ;
		src = L->scratch;
	}
	if (!stored) {
		const u64 d0 = svcGetSystemTick();
		const bool ok = arcDecode(codec, src, comp, dst, raw);
		L->decodeTicks += svcGetSystemTick() - d0;
		if (!ok)
			return LOADER_ERR_READ;
	}
	L->rawBytes += raw;
	return LOADER_ERR_NONE;
}

// LOADER_JOB_HDR from the archive, to the same two ends as from a loose file:
// the header and tables in `tables`, for the main thread to build the model
// from while LOADER_JOB_GEO's pieces wait, or the whole file in `blob`.
static void loaderArcHeader(Loader* L) {
	const ArcSection* a = &L->arc;
	if (!a->off) {
		L->err = LOADER_ERR_MISSING;
		return;
	}
	if (a->raw < MODEL3D_HDR_BYTES) {
		L->err = LOADER_ERR_BAD;
		return;
	}
	if (s_splitLoad && a->pieces == 4 && a->praw[0] >= MODEL3D_HDR_BYTES) {
		u8* t = (u8*)malloc(a->praw[0]);
		if (!t) {
			L->err = LOADER_ERR_HEAP;
			return;
		}
		const int e = arcReadPiece(L, a->off, a->codec, a->comp[0], t, a->praw[0]);
		if (e) {
			free(t);
			L->err = e;
			return;
		}
		memcpy(L->hdr, t, MODEL3D_HDR_BYTES);
		// The pieces have to be where the plan puts the pools, or the GEO job
		// would decode one into a pool of another size.
		const Model3DPlan* p = &L->plan;
		if (model3d_plan(L->hdr, MODEL3D_HDR_BYTES, (long)a->raw, &L->plan) &&
		    !p->texSize && p->oVert == a->praw[0] && p->vertBytes == a->praw[1] &&
		    p->idxBytes == a->praw[2] && p->instBytes == a->praw[3] &&
		    p->instOff + p->instBytes == a->raw) {
			L->tables   = t;
			L->streamed = true;
			L->ok       = true;
			return;
		}
		free(t);
	}
	// The whole file, put back together: the pieces where they sit in it, and
	// the indices' padding, which the archive leaves out, as the zeros it was.
	u32 at[4] = { 0, 0, 0, 0 };
	if (a->pieces == 4) {
		at[1] = a->praw[0];
		at[2] = at[1] + a->praw[1];
		at[3] = a->raw - a->praw[3];
		if (at[2] + a->praw[2] > at[3]) {
			L->err = LOADER_ERR_BAD;
			return;
		}
	}
	u8* b = (u8*)malloc(a->raw);
	if (!b) {
		L->err = LOADER_ERR_HEAP;
		return;
	}
	if (a->pieces == 4)
		memset(b + at[2] + a->praw[2], 0, at[3] - at[2] - a->praw[2]);
	u32 off = a->off;
	for (int k = 0; k < a->pieces; k++) {
		const int e = arcReadPiece(L, off, a->codec, a->comp[k], b + at[k], a->praw[k]);
		if (e) {
			free(b);
			L->err = e;
			return;
		}
		off += a->comp[k];
	}
	L->blob     = b;
	L->blobSize = (long)a->raw;
	L->ok       = true;
}

// LOADER_JOB_GEO from the archive: the vertices and indices decoded straight
// into the pools, then the instance table, which as from a loose file is
// zeroed rather than failing the load when it does not come in.
static void loaderArcGeometry(Loader* L) {
	const ArcSection* a = &L->arc;
	void* vd = model3d_vertex_dest(L->model);
	void* id = model3d_index_dest(L->model);
	if (!vd || !id)
		return;
	u32 off = a->off + a->comp[0];
	int e = arcReadPiece(L, off, a->codec, a->comp[1], vd, a->praw[1]);
	off += a->comp[1];
	if (!e)
		e = arcReadPiece(L, off, a->codec, a->comp[2], id, a->praw[2]);
	off += a->comp[2];
	if (e) {
		L->err = e;
		return;
	}
	L->ok = true;
	u8* inst = (u8*)model3d_inst_dest(L->model);
	if (inst && arcReadPiece(L, off, a->codec, a->comp[3], inst, a->praw[3]))
		memset(inst, 0, a->praw[3]);
}

// A texture's stored chain, already in memory at `src`, into its texture.
static bool arcTexFromMemory(Loader* L, const TexFetch* t, const u8* src) {
	if (t->skip > t->raw || t->bytes > t->raw - t->skip)
		return false;
	const u64 d0 = svcGetSystemTick();
	bool ok;
	if (!t->skip && t->bytes == t->raw) {
		ok = arcDecode(t->codec, src, t->arcBytes, t->dst, t->raw);
	} else if (t->codec == ARC_STORE) {
		ok = t->arcBytes == t->raw;
		if (ok)
			memcpy(t->dst, src + t->skip, t->bytes);
	} else {
		u8* r = (u8*)malloc(t->raw);
		ok = r && arcDecode(t->codec, src, t->arcBytes, r, t->raw);
		if (ok)
			memcpy(t->dst, r + t->skip, t->bytes);
		free(r);
	}
	L->decodeTicks += svcGetSystemTick() - d0;
	return ok;
}

// LOADER_JOB_TEX's reads from the archive, in the archive's order. The
// textures a section is the first to use are stored right behind it in its
// table's order, so they come in as a few long reads through the scratch
// buffer, each as much of the run as it holds, and are decoded out of it.
// The rest -- textures first used by a section that is not resident now --
// are a seek each.
static void loaderArcTextures(Loader* L) {
	int order[LOADER_MAX_TEX];
	int n = 0;
	for (int i = 0; i < L->texCount; i++) {
		TexFetch* t = &L->tex[i];
		t->read = false;
		if (!t->dst)
			continue;
		int k = n++;
		while (k > 0 && L->tex[order[k - 1]].arcOff > t->arcOff) {
			order[k] = order[k - 1];
			k--;
		}
		order[k] = i;
	}
	int i = 0;
	while (i < n) {
		const u32 start = L->tex[order[i]].arcOff;
		u32 end = start;
		int j = i;
		while (j < n) {
			const TexFetch* t = &L->tex[order[j]];
			if (t->arcOff != end || t->arcBytes > L->scratchBytes - (end - start))
				break;
			end += t->arcBytes;
			j++;
		}
		if (j == i) {
			// Bigger than the buffer by itself, which arcOpen() sized so that
			// nothing is; read through heap of its own rather than not at all.
			TexFetch* t = &L->tex[order[i]];
			t->read = arcReadTexture(L->arcFile, t->codec, t->arcOff, t->arcBytes, t->raw,
			                         t->skip, t->dst, t->bytes);
			L->arcPos = ~0u;
			if (t->read) {
				L->jobBytes[LOADER_JOB_TEX] += t->arcBytes;
				L->rawBytes += t->bytes;
				L->texReads++;
			}
			i++;
			continue;
		}
		if (arcSeek(L, start) && arcRead(L, L->scratch, end - start)) {
			L->texReads++;
			for (int k = i; k < j; k++) {
				TexFetch* t = &L->tex[order[k]];
				t->read = arcTexFromMemory(L, t, L->scratch + (t->arcOff - start));
				if (t->read)
					L->rawBytes += t->bytes;
			}
		}
		i = j;
	}
}

// One job of the section in flight, out of WORLD.NFSZ: the header and
// tables, the geometry straight into its pools, or the textures.
static void loaderDoJob(Loader* L) {
	if (L->job == LOADER_JOB_HDR) {
		L->ok       = false;
		L->blob     = NULL;
		L->blobSize = 0;
		L->tables   = NULL;
		L->streamed = false;
		L->err      = LOADER_ERR_NONE;
		L->errNo    = 0;
		loaderArcHeader(L);
		return;
	}

	if (L->job == LOADER_JOB_GEO) {
		// Straight into the two pools the main thread allocated. This is the
		// copy that used to land on the frame: ~1.5 MB on a street tile.
		L->ok = false;
		L->err = LOADER_ERR_READ;
		L->errNo = 0;
		if (L->model)
			loaderArcGeometry(L);
		return;
	}

	// The model's cull cells and physics grid first. Both are CPU work over
	// memory nothing else can see yet, and both are several ms on the big
	// sections (C4 is 30k triangles), which is a dropped frame on the main
	// thread and free here: the worker runs below the main thread, in the time
	// it spends parked on VBlank.
	//
	// Cells before the grid, and only in that order: building them sorts the
	// index pool, and the grid lists triangles by their place in it
	// (model3d_build_cull_cells refuses once the grid exists).
	const u64 c0 = svcGetSystemTick();
	L->cellBytes = model3d_build_cull_cells(L->model);
	L->cellTicks = svcGetSystemTick() - c0;
	const u64 g0 = svcGetSystemTick();
	L->gridBytes = L->physGrid ? model3d_build_phys_grid(L->model) : 0;
	L->gridTicks = svcGetSystemTick() - g0;

	// Straight into the linear memory of the texture the main thread already
	// allocated (bankBeginUpload): no staging buffer, and no copy afterwards.
	// Nothing else can see that memory yet -- the record is `pending`, so the
	// GPU is never told about it -- and the flush that does hand it over is
	// back on the main thread.
	//
	// A read that does not complete is left with `read` false rather than
	// failing the request: the main thread does that one texture itself, and
	// the rest of the section is still ahead of where it would have been.
	L->ok = true;
	loaderArcTextures(L);
}

// loaderDoJob() with its time charged to the job (#270). A merged request
// (#270 P6: the textures were queued at the header's hand-off) runs the
// geometry job and goes straight on to the texture job, and comes back as the
// latter -- or as the former, if the geometry failed.
static void loaderRunJob(Loader* L) {
	for (;;) {
		const int job = L->job;
		const u64 t0 = svcGetSystemTick();
		loaderDoJob(L);
		u64 t = svcGetSystemTick() - t0;
		if (job == LOADER_JOB_TEX)
			t -= L->cellTicks + L->gridTicks;
		if (job >= 0 && job < 3)
			L->jobTicks[job] += t;
		if (job != LOADER_JOB_GEO || !L->ok || !L->merged)
			return;
		L->job = LOADER_JOB_TEX;
	}
}

// What to read ahead once this job is handed back (#270 P6): after a header,
// the rest of the section's pieces; after a section's last job, the pieces of
// the one the main thread expects next. Decided before `done`, which hands
// every field it reads back to the main thread.
static bool loaderAheadPlan(const Loader* L, u32* off, u32* end) {
	if (!L->pfBuf || !L->ok)
		return false;
	const ArcSection* a;
	u32 from;
	if (L->job == LOADER_JOB_HDR) {
		if (!L->streamed)
			return false;
		a = &L->arc;
		from = a->comp[0];
	} else if (L->job == LOADER_JOB_TEX && L->hint >= 0) {
		a = &L->hintArc;
		from = 0;
	} else {
		return false;
	}
	u32 n = 0;
	for (int k = 0; k < a->pieces; k++)
		n += a->comp[k];
	if (!a->off || n <= from || n > L->pfCap)
		return false;
	*off = a->off + from;
	*end = a->off + n;
	return true;
}

// The read-ahead itself, a chunk at a time until it is done or a request is
// waiting: a wrong guess then costs that request at most one chunk. A range
// that carries on from what is already in the buffer is added to it -- after
// a hinted section's header that is all of it, read already -- and anything
// else starts the buffer again.
#define RA_CHUNK (128u * 1024)
static void loaderReadAhead(Loader* L, u32 off, u32 end) {
	if (off < L->pfOff || off - L->pfOff > L->pfHave || end - L->pfOff > L->pfCap) {
		L->pfOff  = off;
		L->pfHave = 0;
	}
	while (L->pfOff + L->pfHave < end && !L->reqPending) {
		const u32 at = L->pfOff + L->pfHave;
		const u32 n = end - at < RA_CHUNK ? end - at : RA_CHUNK;
		const u64 t0 = svcGetSystemTick();
		if (!arcSeek(L, at) || fread(L->pfBuf + L->pfHave, 1, n, L->arcFile) != n) {
			L->arcPos = ~0u;
			return;
		}
		L->arcPos += n;
		L->pfHave += n;
		L->raBytes += n;
		L->raTicks += svcGetSystemTick() - t0;
	}
}

static void loaderThread(void* ud) {
	Loader* L = (Loader*)ud;
	for (;;) {
		LightEvent_Wait(&L->wake);
		L->reqPending = false;
		if (L->quit)
			break;
		loaderRunJob(L);
		u32 off = 0, end = 0;
		const bool ahead = loaderAheadPlan(L, &off, &end);
		LightEvent_Signal(&L->done);
		if (ahead)
			loaderReadAhead(L, off, end);
	}
}

static void loaderStart(World* w) {
	Loader* L = &w->ld;
	LightEvent_Init(&L->wake, RESET_ONESHOT);
	LightEvent_Init(&L->done, RESET_ONESHOT);
	L->hint = -1;
	// arcOpen() has given the worker its handle on WORLD.NFSZ.
	L->physGrid = (w->topo == NULL);
	// On the New 3DS's core 2, which nothing else uses once the boot movies
	// are over (#270 P6). The worker is not only blocked in reads: the cull
	// cells, the physics grid and the GEO job's parse are CPU, and on the main
	// thread's core every millisecond of drawing came out of them -- giving
	// the loading screen a frame every 100 ms made a page-in 0.7 s slower.
	// One step below the main thread's priority still, for when core 2 is
	// refused and it lands back on this one.
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	prio += 1;
	if (prio > 0x3F)
		prio = 0x3F;
	L->thread = threadCreate(loaderThread, L, 16 * 1024, prio, 2, false);
	if (!L->thread) {
		L->thread = threadCreate(loaderThread, L, 16 * 1024, prio, -1, false);
		wlog("world: core 2 refused the loader thread -- it shares the main thread's core");
	}
	if (!L->thread)
		wlog("world: loader thread would not start -- paging will block");
}

static void loaderStop(World* w) {
	Loader* L = &w->ld;
	if (L->thread) {
		// Wait out anything in flight first, so the worker is not freed from
		// under a read it is still doing.
		if (L->busy) {
			LightEvent_Wait(&L->done);
			L->busy = false;
		}
		L->quit = true;
		L->reqPending = true;   // and out of any read-ahead
		LightEvent_Signal(&L->wake);
		threadJoin(L->thread, U64_MAX);
		threadFree(L->thread);
		L->thread = NULL;
	}
	bankCancelPending(w);
	free(L->blob);
	L->blob = NULL;
	free(L->tables);
	L->tables = NULL;
	model3d_free(L->model);
	L->model = NULL;
	if (L->arcFile) {
		fclose(L->arcFile);
		L->arcFile = NULL;
	}
	free(L->scratch);
	L->scratch = NULL;
	L->scratchBytes = 0;
	free(L->pfBuf);
	L->pfBuf = NULL;
	L->pfCap = L->pfOff = L->pfHave = 0;
}

// Bind up to `budget` of the model's texture slots against what the worker
// read. True once every slot is bound.
//
// This used to be the expensive thing left on the main thread once the
// geometry read moved off it (#226): a street tile wants around thirty
// textures, each one a linearAlloc plus a memcpy into it, and together they
// were 4.4 ms of a 4.6 ms commit against a 128px bank -- four times that on
// the shipped 256px one, which is the 43.9 ms the hardware log measured on
// C29. Spreading it over frames was all that could be done about it.
//
// Since #230 the pixels are already in the texture when this runs: the
// allocation happened in queueSectionTextures() and the worker read into it,
// so a slot is a C3D_TexFlush and a refcount. The spread is kept because the
// section still must not be published half-bound, but it is no longer what
// stands between a commit and a dropped frame. Slots shared with a resident
// section were taken when the load was queued and are passed over.
static bool bindSectionTexturesStep(World* w, WorldSection* s, Model3D* model,
                                    int budget) {
	Loader* L = &w->ld;
	if (!w->bankCount || !model)
		return true;
	const int n = model3d_texture_count(model);
	if (n <= 0)
		return true;
	if (!s->texSlots) {
		s->texSlots = (C3D_Tex**)calloc((size_t)n, sizeof(C3D_Tex*));
		if (!s->texSlots)
			return true;   // no slots to fill; the section draws untextured
	}
	for (; L->bindIdx < n && budget > 0; L->bindIdx++) {
		if (s->texSlots[L->bindIdx])
			continue;
		budget--;
		const u32 hash = model3d_texture_hash(model, L->bindIdx);
		const TexFetch* q = NULL;
		for (int k = 0; k < L->texCount; k++) {
			if (L->tex[k].hash == hash) {
				q = &L->tex[k];
				break;
			}
		}
		s->texSlots[L->bindIdx] = bankAcquire(w, hash, q);
	}
	if (L->bindIdx < n)
		return false;
	// What the bank has and could not hand over -- an allocation that failed
	// -- as opposed to a hash it never had, which no amount of memory fixes.
	s->texMissing = 0;
	for (int i = 0; i < n; i++) {
		if (s->texSlots[i])
			continue;
		const u32 h = model3d_texture_hash(model, i);
		const BankTexture* t = h ? bankFind(w, h) : NULL;
		if (t && !t->failed)
			s->texMissing++;
	}
	model3d_set_textures(model, s->texSlots);
	// Every pending record is bound by now -- the queue was built from these
	// same hashes -- so there is nothing left to cancel, only to forget.
	L->texCount = 0;
	return true;
}

static void releaseSectionTextures(World* w, WorldSection* s) {
	if (!s->texSlots)
		return;
	int n = s->model ? model3d_texture_count(s->model) : 0;
	for (int i = 0; i < n; i++)
		bankRelease(w, s->texSlots[i]);
	free(s->texSlots);
	s->texSlots = NULL;
}

static int sectionNumber(const char* name);
#define VIS_MAX_NUMBER    2700u   /* letters A..Z, n < 100 */

World* world_open(const char* dirPath) {
	s_openError[0] = 0;
	if (!dirPath || !*dirPath)
		return NULL;

	char path[400];
	snprintf(path, sizeof(path), "%s/%s", dirPath, WORLD_INDEX);
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(s_openError, sizeof(s_openError), "%s is missing", WORLD_INDEX);
		return NULL;
	}
	u8 hdr[WORLD_HDR];
	if (fread(hdr, 1, WORLD_HDR, f) != WORLD_HDR ||
	    rd32(hdr, 0) != WORLD_MAGIC || rd32(hdr, 4) != WORLD_VERSION ||
	    rd32(hdr, 0x0c) != WORLD_REC) {
		fclose(f);
		return NULL;
	}
	u32 n = rd32(hdr, 8);
	if (!n || n > 4096) {
		fclose(f);
		return NULL;
	}

	World* w = (World*)calloc(1, sizeof(World));
	u8* recs = (u8*)malloc((size_t)n * WORLD_REC);
	if (!w || !recs) {
		free(w);
		free(recs);
		fclose(f);
		return NULL;
	}
	if (fread(recs, 1, (size_t)n * WORLD_REC, f) != (size_t)n * WORLD_REC) {
		free(w);
		free(recs);
		fclose(f);
		return NULL;
	}
	fclose(f);

	w->secs = (WorldSection*)calloc(n, sizeof(WorldSection));
	if (!w->secs) {
		free(w);
		free(recs);
		return NULL;
	}
	snprintf(w->dir, sizeof(w->dir), "%s", dirPath);
	w->count  = (int)n;
	// "No drivable section yet", which world_draw() reads as "draw everything
	// resident". calloc's 0 would be a real index, and without L4RA.BUN there
	// is no secVisible behind it: that was a NULL read on the first frame.
	w->visCurrent = -1;
	w->visPred = -1;
	w->visAhead = -1;
	for (u32 i = 0; i < n; i++) {
		const u8* r = recs + (size_t)i * WORLD_REC;
		WorldSection* s = &w->secs[i];
		s->nextPart = -1;
		memcpy(s->name, r, WORLD_NAME_LEN);
		s->name[WORLD_NAME_LEN - 1] = 0;
		for (int k = 0; k < 3; k++) {
			s->bbmin[k] = rdf32(r, 0x10 + k * 4);
			s->bbmax[k] = rdf32(r, 0x1c + k * 4);
		}
		s->bytes      = rd32(r, 0x28);
		s->numVerts   = rd32(r, 0x2c);
		s->numIdx     = rd32(r, 0x30);
		s->numBatches = rd32(r, 0x34);
		s->memBytes   = s->numVerts && s->numIdx
			? ((s->numVerts * MODEL3D_VERTEX_BYTES + 0x7fu) & ~0x7fu) +
			  ((s->numIdx * 2u + 0x7fu) & ~0x7fu)
			: s->bytes;
		for (int k = 0; k < 3; k++) {
			s->coremin[k] = rdf32(r, 0x40 + k * 4);
			s->coremax[k] = rdf32(r, 0x4c + k * 4);
		}
		s->flags = rd32(r, 0x58);
		if (!(s->flags & SECTION_FAR) && s->numVerts && s->numIdx &&
		    s->memBytes > w->runTarget)
			w->runTarget = s->memBytes;
	}
	// A split section's first part carries its number; the parts after it
	// follow it in and out.
	w->byNumber = (s16*)malloc(VIS_MAX_NUMBER * sizeof(s16));
	if (w->byNumber) {
		for (u32 i = 0; i < VIS_MAX_NUMBER; i++)
			w->byNumber[i] = -1;
		for (u32 i = 0; i < n; i++) {
			const int num = sectionNumber(w->secs[i].name);
			if (num >= 0 && num < (int)VIS_MAX_NUMBER && !strchr(w->secs[i].name, '.'))
				w->byNumber[num] = (s16)i;
		}
	}
	// WORLD.NFSZ is the world's only source (#270, the user's call on
	// 2026-10-26): without it there is nothing to draw or drive on.
	if (!arcOpen(w, hdr, recs)) {
		free(recs);
		free(w->byNumber);
		free(w->secs);
		free(w);
		return NULL;
	}
	free(recs);

	for (int k = 0; k < 3; k++) {
		w->bbmin[k] = rdf32(hdr, 0x10 + k * 4);
		w->bbmax[k] = rdf32(hdr, 0x1c + k * 4);
	}
	// Each step's share of the open, for #270. Topology and scenery first:
	// their 7 MB stay for as long as the world does, so the budget is what
	// is left after them, not before (it was, until 2026-10-25, and the
	// overlays' reserve paid for them).
	const u64 t0 = svcGetSystemTick();
	w->topo = topo_open(dirPath);
	const u64 t1 = svcGetSystemTick();
	w->scenery = scenery_open(dirPath);
	const u64 t2 = svcGetSystemTick();
	w->budget = computeBudget();
	worldArenaOpen(w);
	w->budgetMax = w->budget;
	if (!bankOpen(w)) {
		world_close(w);
		return NULL;
	}
	const u64 t3 = svcGetSystemTick();
	loaderStart(w);
	const u64 t4 = svcGetSystemTick();
	const double k = 1000.0 / (double)SYSCLOCK_ARM11;
	wlog("world: opened -- topology %.0f ms, scenery %.0f ms, bank %.0f ms (%d textures), "
		"loader %.0f ms", (double)(t1 - t0) * k, (double)(t2 - t1) * k,
		(double)(t3 - t2) * k, w->bankCount, (double)(t4 - t3) * k);
	return w;
}

void world_close(World* w) {
	if (!w)
		return;
	loaderStop(w);
	for (int i = 0; i < w->count; i++) {
		releaseSectionTextures(w, &w->secs[i]);
		model3d_free(w->secs[i].model);
	}
	bankClose(w);
	arcClose(w);
	if (arena_active()) {
		wlog("world: compaction moved %u KB in %u step(s) this visit", w->compactBytes >> 10,
			w->compactSteps);
		if (arena_blocks())
			wlog("world: closing the arena with %d block(s), %u KB, still in it",
				arena_blocks(), (unsigned)(arena_used() >> 10));
		arena_destroy();
	}
	topo_close(w->topo);
	scenery_close(w->scenery);
	free(w->vis);
	free(w->visPts);
	free(w->visList);
	free(w->secVisible);
	free(w->visNeigh);
	free(w->byNumber);
	free(w->secs);
	free(w);
}

u32 world_section_serial(const World* w, int number) {
	if (!w || !w->byNumber || number < 0 || number >= (int)VIS_MAX_NUMBER)
		return 0;
	const s16 i = w->byNumber[number];
	return i >= 0 && w->secs[i].model ? w->secs[i].serial : 0;
}

bool world_find_instance(const World* w, int number, const float pos[3],
		Model3D** outModel, u32* outInst) {
	if (!w || !w->byNumber || number < 0 || number >= (int)VIS_MAX_NUMBER)
		return false;
	// Every part of the section: track_pack.py splits a large one at its
	// index pool, and an instance lies wholly in one of them.
	for (s16 i = w->byNumber[number]; i >= 0 && i < w->count; i = w->secs[i].nextPart) {
		if (w->secs[i].model && model3d_find_instance(w->secs[i].model, pos, outInst)) {
			*outModel = w->secs[i].model;
			return true;
		}
	}
	return false;
}

int world_instance_lod(const World* w, int number, const float pos[3], u32* serial,
		Model3D** model, u32* inst) {
	const u32 s = world_section_serial(w, number);
	if (!s) {
		*serial = 0;
		*model = NULL;
		return 0;
	}
	if (s != *serial) {
		if (s_instLooks <= 0)
			return 0;
		s_instLooks--;
		*serial = s;
		*model = NULL;
		if (!world_find_instance(w, number, pos, model, inst)) {
			*model = NULL;
			return -1;
		}
	}
	return *model ? model3d_instance_lod(*model, *inst, s_drawLodFrame) : 0;
}

Model3D* world_anim_part(const World* w, int part, u32* serial) {
	*serial = 0;
	if (!w)
		return NULL;
	for (int i = 0; i < w->count; i++) {
		if (!(w->secs[i].flags & SECTION_ANIM))
			continue;
		if (part-- > 0)
			continue;
		if (!w->secs[i].model)
			return NULL;
		*serial = w->secs[i].serial;
		return w->secs[i].model;
	}
	return NULL;
}

int world_anim_parts(const World* w) {
	int n = 0;
	for (int i = 0; w && i < w->count; i++)
		if (w->secs[i].flags & SECTION_ANIM)
			n++;
	return n;
}

void world_set_prop_draw(WorldPropDraw fn, void* ud) {
	s_propDraw = fn;
	s_propDrawUd = ud;
}

int world_topo_section(const World* w, int coord) {
	return w ? topo_coord_tree_section(w->topo, coord) : -1;
}

bool world_section_loaded(const World* w, int number) {
	if (!w || !w->byNumber || number < 0 || number >= (int)VIS_MAX_NUMBER)
		return false;
	const s16 i = w->byNumber[number];
	return i >= 0 && w->secs[i].model != NULL;
}

void world_set_groups(World* w, int trackId, int careerStage,
		bool hasCareerCar, bool reverse) {
	if (!w)
		return;
	topo_set_groups(w->topo, trackId, careerStage);
	scenery_set_groups(w->scenery, trackId, careerStage, hasCareerCar, reverse,
		w->sceneryOn);
}

static void dropSection(World* w, int i) {
	WorldSection* s = &w->secs[i];
	if (!s->model)
		return;
	const u64 d0 = svcGetSystemTick();
	const int nTex = model3d_texture_count(s->model);
	releaseSectionTextures(w, s);
	model3d_free(s->model);
	s->model = NULL;
	w->resident--;
	if (!(s->flags & SECTION_FAR))
		w->residentNear--;
	w->residentBytes -= s->memBytes;
	// Reported for the same reason the load is: it lands on a frame, and
	// several sections can go in one of them.
	wlog("world: drop %s (%d resident, %u+%u KB) %.1f ms, %d slots",
		s->name, w->resident,
		w->residentBytes >> 10, w->bankBytes >> 10,
		(double)(svcGetSystemTick() - d0) * 1000.0 / (double)SYSCLOCK_ARM11,
		nTex);
}

// What a subset whose texture hash resolves to nothing is painted (TASKS.md
// #136, #138). When this was written 36 hashes were missing, almost all the
// `TRN_*_ROADA_*` road surface; the texture index has since found them all
// (0xd1c901ef is RDP_LANEA_YS_AA_KT in section Y0's pack) and the world now
// resolves 1596 of its 1602 hashes. The six left -- 0x496e7bdd, 0x5c790380,
// 0x6dae9ac9, 0x7bd9c2fc, 0x828dcf0b, 0x931d03a3, 237 triangles on the
// XB_TRAILORA / XB_FACTORYELEMENTG / XB_FACTORYDBUILDINGD / XB_POWER_TOWERA
// props -- are in no pack retail loads either. Retail's lookup on a miss,
// GetTextureInfo(hash, 1, 0) (GC 0x800CFE54), hands back its default
// TextureInfo, a placeholder rather than art; the port draws that placeholder
// as this flat grey, which suits the concrete and metal those props are.
//
// Painted rather than left on vertex colour alone because the baked colour
// is a mid grey around 0.6 and scene.v.pica's exposure lift is calibrated to
// sit under a dark texture: with none to darken it the surface clips to 1.0
// and reads white. Also what a slot shows while texBackfill() retries it.
#define MISSING_TEXTURE_PAINT 0xFF56585C   // ABGR, so rgb(0x5c,0x58,0x56): flat, faintly warm grey

// Hand the current request to the worker. With no worker -- threadCreate()
// failed -- the job runs here and signals its own completion, so the state
// machine above it is identical either way and simply never gets a frame off.
static void submitJob(World* w) {
	Loader* L = &w->ld;
	L->busy = true;
	if (L->thread) {
		L->reqPending = true;
		LightEvent_Signal(&L->wake);
	} else {
		loaderRunJob(L);
		LightEvent_Signal(&L->done);
	}
}

static void submitSection(World* w, int i) {
	Loader* L = &w->ld;
	WorldSection* s = &w->secs[i];
	L->sec        = i;
	L->job        = LOADER_JOB_HDR;
	L->texCount   = 0;
	L->mainTicks  = 0;
	memset(L->jobTicks, 0, sizeof(L->jobTicks));
	memset(L->jobBytes, 0, sizeof(L->jobBytes));
	L->texReads   = 0;
	L->decodeTicks = 0;
	L->rawBytes   = 0;
	L->aheadBytes = 0;
	L->merged     = false;
	if (L->hint >= 0 && i != L->hint) {
		const WorldSection* h = &w->secs[L->hint];
		wlog("world: read-ahead guessed %s (tier %d%s%s), asked for %s (tier %d)",
			h->name, h->tier, h->model ? ", in" : "", h->failed ? ", failed" : "",
			s->name, s->tier);
	}
	L->hinted     = i == L->hint;   // and so probably read ahead already
	L->hint       = -1;
	L->arc        = w->arcSec[i];
	L->submitTick = svcGetSystemTick();
	s->loading = true;
	w->submitted = true;
	submitJob(w);
}

static void streamShrink(World* w, const char* why);

static void abandonLoad(World* w, WorldSection* s, int err) {
	Loader* L = &w->ld;
	free(L->blob);
	L->blob = NULL;
	free(L->tables);
	L->tables = NULL;
	// A load abandoned after queueSectionTextures() owns allocated textures
	// that nothing will ever bind, and holds references on the shared ones.
	if (s->texSlots) {
		const int n = L->model ? model3d_texture_count(L->model) : 0;
		for (int i = 0; i < n; i++)
			bankRelease(w, s->texSlots[i]);
		free(s->texSlots);
		s->texSlots = NULL;
	}
	model3d_free(L->model);
	L->model = NULL;
	bankCancelPending(w);
	L->binding = false;
	L->bindIdx = 0;
	s->loading = false;
	const bool memory = err == LOADER_ERR_HEAP || err == LOADER_ERR_LINEAR ||
		(err == LOADER_ERR_OPEN && L->errNo == ENOMEM);
	if (err == LOADER_ERR_MISSING || err == LOADER_ERR_BAD ||
	    (!memory && ++s->retries > STREAM_RETRIES)) {
		s->failed = true;
		wlog("world: %s MISSING or invalid (%s, errno %d) -- skipped", s->name,
			loaderErrName(err), L->errNo);
		return;
	}
	// The file is fine; the moment was not. What the heaps had left says
	// which -- and it is only worth knowing here, where something failed.
	s->retryFrame = w->frame + STREAM_RETRY_FRAMES;
	wlog("world: %s did not load (%s, errno %d) -- linear %u KB free, %u KB largest, "
		"heap %u KB free, world %u+%u of %u KB; trying again", s->name,
		loaderErrName(err), L->errNo, (unsigned)(linearSpaceFree() >> 10),
		(unsigned)(heap_linear_largest() >> 10), (unsigned)(heap_malloc_free() >> 10),
		w->residentBytes >> 10, w->bankBytes >> 10, w->budget >> 10);
	if (arena_active())
		wlog("world: the arena then: %u of %u KB used, %u KB largest, %d blocks",
			(unsigned)(arena_used() >> 10), (unsigned)(arena_size() >> 10),
			(unsigned)(arena_largest() >> 10), arena_blocks());
	if (memory)
		streamShrink(w, s->name);
}

// Publish a finished section: bind its textures and put it in the resident
// set. Everything here is main-thread work and measures in single-digit ms.
static bool commitSection(World* w) {
	Loader* L = &w->ld;
	WorldSection* s = &w->secs[L->sec];
	const u64 m0 = svcGetSystemTick();

	if (!L->binding) {
		L->bindIdx   = 0;
		L->bindTicks = 0;
		L->binding   = true;
	}
	// The model is handed over only once every slot is bound. A section is
	// drawable the moment s->model is set, so publishing it half-bound would
	// flash the unbound half in MISSING_TEXTURE_PAINT -- and it would also put
	// it in reach of dropSection(), which bails on a section with no model and
	// would otherwise free what is being bound into.
	const u64 b0 = svcGetSystemTick();
	const bool bound =
		bindSectionTexturesStep(w, s, L->model, BIND_PER_FRAME);
	L->bindTicks += svcGetSystemTick() - b0;
	if (!bound) {
		mainCost(L, svcGetSystemTick() - m0);
		return false;   // the remaining slots get the frames after this one
	}
	L->binding = false;
	s->model = L->model;
	L->model = NULL;
	s->serial = ++s_sectionSerial;
	s->loading = false;
	// A section read ahead that is never wanted ages from here.
	s->lastWanted = w->frame;
	w->streamFull = false;
	w->resident++;
	if (!(s->flags & SECTION_FAR))
		w->residentNear++;
	w->residentBytes += s->memBytes;

	mainCost(L, svcGetSystemTick() - m0);
	const u64 now = svcGetSystemTick();
	// phys/total is the collision cutoff the blob parse arrived at. It is here
	// rather than behind a debug flag because the two ways it can be wrong are
	// both silent: too high and street signage stays solid (#183), too low and
	// real geometry -- a guardrail, a wall -- quietly stops colliding.
	int physIdx = 0, totalIdx = 0;
	model3d_phys_span(s->model, &physIdx, &totalIdx);
	int gw = 0, gh = 0, gRefs = 0;
	float gCell = 0.0f;
	model3d_phys_grid_info(s->model, &gw, &gh, &gCell, &gRefs);
	// The cull table (#68) is reported for the same reason the physics grid is:
	// both are built on the worker out of sight, both cost linear memory the
	// budget has to hold, and a section that silently came back with no cells
	// is drawn whole -- which looks exactly like the cull not helping.
	int cCells = 0;
	float cStep = 0.0f;
	model3d_cull_info(s->model, &cCells, &cStep);
	// A section packed with an instance table (#247) is culled by its
	// instances instead, and reports those in place of cells.
	int nInst = 0, nRuns = 0, nBoxes = 0;
	model3d_inst_info(s->model, &nInst, &nRuns, &nBoxes);
	// How many of this section's textures the bank had to upload, as opposed
	// to handing back one a resident section already paid for -- and how many
	// of those the worker did not read, which on the card is a frame each.
	const int nBound = L->boundCount;
	wlog("world: load %s (%d resident, %u+%u/%u KB) %.0f ms wall, %.1f ms on "
		"the main thread, worst frame %.1f ms (%.1f ms binding %d "
		"textures, %d sync), phys %d/%d, "
		"grid %dx%d@%.0fm %d refs %u KB %.1f ms, "
		"cull %d cells@%.0fm %d inst %d runs %d boxes %u KB %.1f ms",
		s->name, w->resident,
		w->residentBytes >> 10, w->bankBytes >> 10, w->budget >> 10,
		(double)(now - L->submitTick) * 1000.0 / (double)SYSCLOCK_ARM11,
		(double)L->mainTicks * 1000.0 / (double)SYSCLOCK_ARM11,
		(double)L->mainWorst * 1000.0 / (double)SYSCLOCK_ARM11,
		(double)L->bindTicks * 1000.0 / (double)SYSCLOCK_ARM11, nBound,
		L->syncCount, physIdx, totalIdx, gw, gh, (double)gCell, gRefs,
		(unsigned)(L->gridBytes >> 10),
		(double)L->gridTicks * 1000.0 / (double)SYSCLOCK_ARM11,
		cCells, (double)cStep, nInst, nRuns, nBoxes,
		(unsigned)(L->cellBytes >> 10),
		(double)L->cellTicks * 1000.0 / (double)SYSCLOCK_ARM11);
	// And the wall time split by job (#270), on a line of its own because the
	// one above is already at wlog()'s length. `idle` is whatever the worker
	// was not running a job: the hand-offs, and the main thread's share.
	{
		const double k = 1000.0 / (double)SYSCLOCK_ARM11;
		const u64 busy = L->jobTicks[0] + L->jobTicks[1] + L->jobTicks[2] +
			L->cellTicks + L->gridTicks;
		const u64 wall = now - L->submitTick;
		const u32 bytes = L->jobBytes[0] + L->jobBytes[1] + L->jobBytes[2];
		const u64 io = L->jobTicks[0] + L->jobTicks[1] + L->jobTicks[2];
		// The KB are what came off the card; with an archive, `decode` is the
		// share of the job time spent unpacking them into what they make.
		// `ahead` is how many of the KB were read ahead, outside the jobs
		// (#270 P6), and `ra` the read-ahead's running total, used or not.
		wlog("world: io %s hdr %.0f ms %u KB, geo %.0f ms %u KB, tex %.0f ms %d reads "
			"%u KB, %.2f MB/s, idle %.0f ms, decode %.0f ms %u KB, ahead %u KB, "
			"ra %u KB %.0f ms",
			s->name, (double)L->jobTicks[0] * k, (unsigned)(L->jobBytes[0] >> 10),
			(double)L->jobTicks[1] * k, (unsigned)(L->jobBytes[1] >> 10),
			(double)L->jobTicks[2] * k, L->texReads, (unsigned)(L->jobBytes[2] >> 10),
			io ? (double)bytes / ((double)io * k * 1000.0) : 0.0,
			wall > busy ? (double)(wall - busy) * k : 0.0,
			(double)L->decodeTicks * k, (unsigned)(L->rawBytes >> 10),
			(unsigned)(L->aheadBytes >> 10), (unsigned)(L->raBytes >> 10),
			(double)L->raTicks * k);
	}
	if (s->texMissing) {
		wlog("world: %s has %u texture(s) that did not allocate -- linear %u KB free, "
			"%u KB largest", s->name, (unsigned)s->texMissing,
			(unsigned)(linearSpaceFree() >> 10), (unsigned)(heap_linear_largest() >> 10));
		streamShrink(w, s->name);
	}
	L->bindTicks = 0;
	L->boundCount = 0;
	L->syncCount = 0;
	L->mainWorst = 0;
	L->gridBytes = 0;
	L->gridTicks = 0;
	L->cellBytes = 0;
	L->cellTicks = 0;
	return true;
}

// Allocate the textures this section needs and the bank does not already
// hold, then hand the request on for the worker to read into them. Textures
// shared with a section already resident cost nothing and are not queued --
// on a typical street tile most of them are.
//
// Those shared ones are taken here, not at bind time. The pager drops
// sections every frame, including the frames this load spends on the worker,
// and a texture whose last other holder went in that window was freed under
// the load: nothing had queued it, so the bind read it back synchronously.
// That was every bind spike over 15 ms in the user's 10-23 card logs (Q28 at
// 23.4 ms for two textures, P30 19.9, N29 17.5, P28 17.2; O34 37.9 and R7
// 37.1 the run before), each one a 256px chain or two read off the card on
// the main thread. A reference held from here on keeps the texture resident
// until the section either commits or is closed with the world.
//
// The allocations are the main thread's share of #230 and all of it: the loop
// is a C3D_TexInitWithParams per new texture, which is a linearAlloc and some
// field setting. A hundred of them are charged to mainCost() below, so if that
// ever stops being free the load line says so.
static int streamVictim(const World* w, bool forWanted, const float pos[3]);

// MakeSpaceInPool for a section's textures (#270): what its new ones will add
// to the bank, made room for before any of them is allocated. The budget check
// before the load could count only the geometry -- which textures a section
// needs that the bank does not already hold is in its tables, read since --
// and on 2026-10-25 C22 came in with 38 of its textures past the arena's edge
// and drew them grey until texBackfill() caught up. Unwanted sections go
// first, read-ahead ones too for a wanted section, as for its geometry. The
// radius pager has no tiers to choose by and keeps its old behaviour.
static void streamMakeTexRoom(World* w, const WorldSection* s, u32 newBytes) {
	if (!w->vis || w->visCurrent < 0)
		return;
	const bool wanted = s->tier == STREAM_WANTED;
	while (w->residentBytes + s->memBytes + w->bankBytes + newBytes > w->budget) {
		const int v = streamVictim(w, wanted, w->lastPos);
		if (v < 0)
			return;
		dropSection(w, v);
	}
}

static int streamPick(const World* w, const float pos[3]);

// `job` is the one to hand the request on as: LOADER_JOB_TEX once the model
// has its geometry, or LOADER_JOB_GEO straight from the header, which the
// worker then takes on to the textures without coming back first (#270 P6).
// The tables say which textures a section needs before its vertices are in.
static void queueSectionTextures(World* w, int job) {
	Loader* L = &w->ld;
	WorldSection* s = &w->secs[L->sec];
	const u64 m0 = svcGetSystemTick();
	L->texCount = 0;
	const int n = w->bankCount ? model3d_texture_count(L->model) : 0;
	// No slots is not fatal here: bindSectionTexturesStep() tries again and
	// draws the section untextured if that fails too.
	if (n > 0 && !s->texSlots)
		s->texSlots = (C3D_Tex**)calloc((size_t)n, sizeof(C3D_Tex*));
	// What the bank holds already is taken first -- a reference keeps it
	// through the evictions below -- and what it does not is added up.
	u32 newBytes = 0;
	for (int i = 0; i < n; i++) {
		const u32 h = model3d_texture_hash(L->model, i);
		BankTexture* t = h ? bankFind(w, h) : NULL;
		if (!t || t->failed || t->pending)
			continue;
		if (t->valid) {
			if (s->texSlots && !s->texSlots[i])
				s->texSlots[i] = bankAcquire(w, h, NULL);
			continue;
		}
		if (!bankRecordSane(t))
			continue;
		bool seen = false;
		for (int k = 0; k < i && !seen; k++)
			seen = model3d_texture_hash(L->model, k) == h;
		if (!seen) {
			BankUpload u;
			bankUploadPlan(w, t, &u);
			newBytes += u.bytes;
		}
	}
	streamMakeTexRoom(w, s, newBytes);
	for (int i = 0; i < n; i++) {
		const u32 h = model3d_texture_hash(L->model, i);
		if (!h)
			continue;
		BankTexture* t = bankFind(w, h);
		if (!t || t->failed || t->pending || t->valid)
			continue;
		if (L->texCount >= LOADER_MAX_TEX || !bankRecordSane(t))
			continue;
		bool queued = false;
		for (int k = 0; k < L->texCount; k++)
			if (L->tex[k].hash == h) { queued = true; break; }
		if (queued)
			continue;
		TexFetch* q = &L->tex[L->texCount];
		q->hash = h;
		// Allocating the texture here, rather than at bind time, is what lets
		// the worker read into it instead of into a staging buffer (#230).
		// One that will not allocate is simply not queued:
		// bindSectionTexturesStep() reaches it anyway and tries again, which
		// is where linear pressure was always handled.
		if (!bankBeginUpload(w, t, q))
			continue;
		L->texCount++;
	}
	// This is the section's last job, so the worker is told what to read
	// ahead after it: what the pager would ask for next if this one were in
	// already (#270 P6). A different ask when the time comes -- the car moved
	// on, or the pick does not fit -- costs the read-ahead and no more.
	L->hint = -1;
	if (w->arcSec && L->pfBuf && w->retailPick) {
		const int h = streamPick(w, w->lastPos);
		if (h >= 0) {
			L->hint = h;
			L->hintArc = w->arcSec[h];
		}
	}
	mainCost(L, svcGetSystemTick() - m0);

	// Always, even with no pixels to fetch: the same job builds the model's
	// cull cells and physics grid.
	L->merged = job == LOADER_JOB_GEO;
	L->job = job;
	submitJob(w);
}

// One completed request, taken one step forward.
static bool advanceLoad(World* w) {
	Loader* L = &w->ld;
	WorldSection* s = &w->secs[L->sec];

	if (L->job == LOADER_JOB_HDR) {
		const u64 m0 = svcGetSystemTick();
		if (!L->ok) {
			abandonLoad(w, s, L->err);
			return false;
		}
		// Allocate the model and both GPU pools and fill in everything the
		// header and tables describe. On the streamed path the vertices and
		// indices are not here yet -- the next job reads them straight into
		// the pools this just allocated, which is the point of the split.
		const ArenaSide was = arena_route(ARENA_HIGH);
		const u32 tag = arena_tag(TAG_SECTION | (u32)L->sec);
		L->model = L->streamed
			? model3d_from_plan(&L->plan, L->tables)
			: model3d_from_memory(L->blob, (size_t)L->blobSize);
		arena_tag(tag);
		arena_route(was);
		free(L->tables);
		L->tables = NULL;
		free(L->blob);
		L->blob = NULL;
		if (!L->model) {
			// model3d_from_plan() is a linearAlloc() for each pool and a
			// few mallocs; the plan itself was checked on the worker.
			abandonLoad(w, s, LOADER_ERR_LINEAR);
			return false;
		}
		model3d_set_untextured_paint(L->model, MISSING_TEXTURE_PAINT);
		// Before the worker builds the physics grid, which reads it.
		model3d_set_scenery(L->model, w->sceneryOn);
		mainCost(L, svcGetSystemTick() - m0);

		// The streamed path's geometry is still to come; the worker reads it
		// and goes on to the textures in one request. The whole-file path
		// already has it.
		queueSectionTextures(w, L->streamed ? LOADER_JOB_GEO : LOADER_JOB_TEX);
		return false;
	}

	if (L->job == LOADER_JOB_GEO) {
		// A merged request back as this one is a geometry job that failed.
		if (!L->ok || !L->model || L->merged) {
			abandonLoad(w, s, L->ok ? LOADER_ERR_READ : L->err);
			return false;
		}
		queueSectionTextures(w, LOADER_JOB_TEX);
		return false;
	}

	return commitSection(w);
}

// ---- Retail section visibility (TASKS.md #191) ------------------------------
//
// Which sections the engine draws is not a radius. L4RA.BUN's 0x80034150
// chunk is the retail VisibleSectionManager's data, and it says, for each of
// the track's 255 *drivable* sections, exactly which sections are visible
// from inside it:
//
//   0x00034152  boundaries, one variable-length record per section:
//                 +0x08 u16 section number   (letter * 100 + n, C13 -> 313)
//                 +0x0a u8  point count
//                 +0x0c float2 bbox min, float2 bbox max, float2 centre
//                 +0x24 float2 points[count]  -- the outline, world x/y
//   0x00034153  drivable sections, 164 bytes each:
//                 +0x08 u16 section number
//                 +0x10 u32 visible count (at most 72)
//                 +0x14 u16 visible[count] -- section numbers
//
// The drivable sections are logical regions with no geometry of their own
// (C13, the one around the C59 plaza, has no entry in the stream table at
// all); their lists name the geometry sections to draw, plus the texture-only
// X/Y sections, which this port has no use for.
//
// This is what fixes #191's ground, and the user's "building in the middle of
// the road". The port used to draw every pinned far-scenery section from
// everywhere, on the reading that a skyline and distant-LOD shell is "meant
// to be visible from anywhere". The retail lists say otherwise. A99, a
// 3071-vertex low-detail shell of the whole city, is visible from 26 of the
// 255 drivable sections; E98 from 103 and E99 from 65. Drawn from the C59
// plaza, where none of the three is listed, A99's coarse ground lay over the
// real road and washed it out to a smooth pale pink, and its block-out of
// the next street stood in the middle of the road. C13's list has A98 and
// G99 and none of those three.
//
// The pager is still distance-based: this decides what is *drawn* out of
// what is resident, not what is loaded.
#define VIS_GROUP_TAG     0x80034150u
#define VIS_BOUNDARY_TAG  0x00034152u
#define VIS_DRIVABLE_TAG  0x00034153u
#define VIS_DRIVABLE_REC  164u
#define VIS_MAX_VISIBLE   72u
#define VIS_MAX_FILE      (4u * 1024 * 1024)

// The retail number of an index section ("C59" -> 359), or -1 for a name
// that is not one letter and a number. A part of a split section ("C4.1",
// see WorldSection::nextPart) has its section's number.
static int sectionNumber(const char* name) {
	if (name[0] < 'A' || name[0] > 'Z' || name[1] < '0' || name[1] > '9')
		return -1;
	int n = 0;
	const char* p = name + 1;
	for (; *p && *p != '.'; p++) {
		if (*p < '0' || *p > '9' || n > 99)
			return -1;
		n = n * 10 + (*p - '0');
	}
	if (*p == '.') {
		if (!p[1])
			return -1;
		for (const char* q = p + 1; *q; q++)
			if (*q < '0' || *q > '9')
				return -1;
	}
	return n > 99 ? -1 : (name[0] - 'A' + 1) * 100 + n;
}

// Whether two drivable sections' outlines come within STREAM_NEIGHBOUR_GAP of
// each other: a corner of `a` near an edge of `b`. Called both ways round, at
// load, for the pairs whose boxes already touch.
static float segDistSq(float px, float py, float ax, float ay, float bx, float by) {
	const float ex = bx - ax, ey = by - ay;
	const float l = ex * ex + ey * ey;
	float t = l > 0.0f ? ((px - ax) * ex + (py - ay) * ey) / l : 0.0f;
	if (t < 0.0f)
		t = 0.0f;
	else if (t > 1.0f)
		t = 1.0f;
	const float dx = ax + ex * t - px, dy = ay + ey * t - py;
	return dx * dx + dy * dy;
}

static bool outlineNear(const VisRegion* a, const VisRegion* b, const float* pts) {
	const float g = STREAM_NEIGHBOUR_GAP;
	const float* pa = pts + 2 * a->firstPoint;
	const float* pb = pts + 2 * b->firstPoint;
	for (u32 i = 0; i < a->numPoints; i++) {
		const float x = pa[2 * i], y = pa[2 * i + 1];
		if (x < b->bbmin[0] - g || x > b->bbmax[0] + g ||
		    y < b->bbmin[1] - g || y > b->bbmax[1] + g)
			continue;
		for (u32 k = 0, j = b->numPoints - 1u; k < b->numPoints; j = k++)
			if (segDistSq(x, y, pb[2 * j], pb[2 * j + 1], pb[2 * k], pb[2 * k + 1]) <= g * g)
				return true;
	}
	return false;
}

bool world_load_visibility(World* w, const char* bunPath) {
	if (!w || !bunPath)
		return false;
	FILE* f = fopen(bunPath, "rb");
	if (!f) {
		wlog("world: no %s -- every resident section is drawn (copy L4RA.BUN from the PC install)", bunPath);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* d = (size > 8 && size <= (long)VIS_MAX_FILE) ? (u8*)malloc((size_t)size) : NULL;
	if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) {
		free(d);
		fclose(f);
		wlog("world: could not read %s (%ld bytes)", bunPath, size);
		return false;
	}
	fclose(f);

	// Find the group, then its two leaves.
	u32 bOff = 0, bLen = 0, dOff = 0, dLen = 0;
	for (u32 o = 0; o + 8 <= (u32)size; ) {
		const u32 id = rd32(d, o), len = rd32(d, o + 4);
		if (len > (u32)size - o - 8)
			break;
		if (id == VIS_GROUP_TAG) {
			for (u32 c = o + 8; c + 8 <= o + 8 + len; ) {
				const u32 cid = rd32(d, c), clen = rd32(d, c + 4);
				if (clen > o + 8 + len - c - 8)
					break;
				if (cid == VIS_BOUNDARY_TAG) { bOff = c + 8; bLen = clen; }
				if (cid == VIS_DRIVABLE_TAG) { dOff = c + 8; dLen = clen; }
				c += 8 + clen;
			}
			break;
		}
		o += 8 + len;
	}
	const u32 nDrive = dLen / VIS_DRIVABLE_REC;
	if (!bLen || !nDrive) {
		free(d);
		wlog("world: %s has no visible-section data", bunPath);
		return false;
	}

	// Index section by retail number, so a list entry resolves in one step.
	// A split section's parts hang off the section itself (nextPart), so
	// each list entry for it brings its parts along: before #270 they had no
	// number, were loaded by the radius pager and then never drawn.
	u32 parts = 0;
	for (int i = 0; i < w->count; i++)
		if (strchr(w->secs[i].name, '.'))
			parts++;
	s16* byNumber = (s16*)malloc(VIS_MAX_NUMBER * sizeof(s16));
	VisRegion* regs = (VisRegion*)calloc(nDrive, sizeof(VisRegion));
	s16* list = (s16*)malloc((size_t)nDrive * (VIS_MAX_VISIBLE + parts) * sizeof(s16));
	float* pts = (float*)malloc((bLen / 8) * 2 * sizeof(float));
	u8* secVis = (u8*)calloc((size_t)w->count, 1);
	if (!byNumber || !regs || !list || !pts || !secVis) {
		free(byNumber); free(regs); free(list); free(pts); free(secVis); free(d);
		return false;
	}
	for (u32 i = 0; i < VIS_MAX_NUMBER; i++)
		byNumber[i] = -1;
	for (int i = 0; i < w->count; i++) {
		w->secs[i].nextPart = -1;
		const int num = sectionNumber(w->secs[i].name);
		if (num >= 0 && num < (int)VIS_MAX_NUMBER && !strchr(w->secs[i].name, '.'))
			byNumber[num] = (s16)i;
	}
	u32 linked = 0;
	for (int i = 0; i < w->count; i++) {
		if (!strchr(w->secs[i].name, '.'))
			continue;
		const int num = sectionNumber(w->secs[i].name);
		const s16 base = num >= 0 && num < (int)VIS_MAX_NUMBER ? byNumber[num] : -1;
		if (base < 0)
			continue;
		s16* link = &w->secs[base].nextPart;
		while (*link >= 0)
			link = &w->secs[*link].nextPart;
		*link = (s16)i;
		linked++;
	}

	u32 nList = 0, unknown = 0;
	for (u32 i = 0; i < nDrive; i++) {
		const u8* r = d + dOff + i * VIS_DRIVABLE_REC;
		VisRegion* g = &regs[i];
		g->number = (u16)(rd32(r, 0x08) & 0xffff);
		u32 nv = rd32(r, 0x10);
		if (nv > VIS_MAX_VISIBLE)
			nv = VIS_MAX_VISIBLE;
		g->firstVisible = nList;
		for (u32 k = 0; k < nv; k++) {
			const u32 num = (u32)r[0x14 + k * 2] | ((u32)r[0x15 + k * 2] << 8);
			const s16 idx = num < VIS_MAX_NUMBER ? byNumber[num] : -1;
			if (idx >= 0) {
				for (s16 p = idx; p >= 0; p = w->secs[p].nextPart)
					list[nList++] = p;
			} else {
				unknown++;   // a texture-only X/Y section, or a drivable key
			}
		}
		g->numVisible = nList - g->firstVisible;
	}

	// Outlines, kept only for sections that are drivable.
	u32 nPts = 0, outlined = 0;
	for (u32 o = bOff; o + 0x24 <= bOff + bLen; ) {
		const u16 num = (u16)(rd32(d, o + 0x08) & 0xffff);
		const u32 np = d[o + 0x0a];
		if (o + 0x24 + np * 8 > bOff + bLen)
			break;
		for (u32 i = 0; i < nDrive; i++) {
			VisRegion* g = &regs[i];
			if (g->number != num || g->numPoints || np < 3)
				continue;
			g->bbmin[0] = rdf32(d, o + 0x0c);
			g->bbmin[1] = rdf32(d, o + 0x10);
			g->bbmax[0] = rdf32(d, o + 0x14);
			g->bbmax[1] = rdf32(d, o + 0x18);
			g->firstPoint = nPts;
			g->numPoints = (u16)np;
			for (u32 k = 0; k < np * 2; k++)
				pts[nPts * 2 + k] = rdf32(d, o + 0x24 + k * 4);
			nPts += np;
			outlined++;
			break;
		}
		o += 0x24 + np * 8;
	}
	free(byNumber);
	free(d);

	// Which drivable sections touch which, for the streamer to read their
	// lists ahead (#270 P1). Retail has no use for it, so it is not in the
	// file; it is worked out from the outlines once, here.
	const float gap = STREAM_NEIGHBOUR_GAP;
	u32 nNeigh = 0, capNeigh = nDrive * 8;
	u16* neigh = (u16*)malloc(capNeigh * sizeof(u16));
	for (u32 i = 0; neigh && i < nDrive; i++) {
		VisRegion* a = &regs[i];
		a->firstNeigh = nNeigh;
		for (u32 k = 0; a->numPoints && k < nDrive; k++) {
			const VisRegion* b = &regs[k];
			if (k == i || !b->numPoints ||
			    a->bbmin[0] > b->bbmax[0] + gap || b->bbmin[0] > a->bbmax[0] + gap ||
			    a->bbmin[1] > b->bbmax[1] + gap || b->bbmin[1] > a->bbmax[1] + gap)
				continue;
			if (!outlineNear(a, b, pts) && !outlineNear(b, a, pts))
				continue;
			if (nNeigh == capNeigh) {
				u16* grown = (u16*)realloc(neigh, (size_t)capNeigh * 2 * sizeof(u16));
				if (!grown) {
					free(neigh);
					neigh = NULL;
					break;
				}
				neigh = grown;
				capNeigh *= 2;
			}
			neigh[nNeigh++] = (u16)k;
		}
		a->numNeigh = nNeigh - a->firstNeigh;
	}
	if (!neigh) {
		// Then nothing is read ahead but the point STREAM_AHEAD_S out.
		nNeigh = 0;
		for (u32 i = 0; i < nDrive; i++)
			regs[i].firstNeigh = regs[i].numNeigh = 0;
	}

	w->vis = regs;
	w->visCount = (int)nDrive;
	w->visPts = pts;
	w->visList = list;
	w->visCurrent = -1;
	w->visPred = -1;
	w->visAhead = -1;
	w->secVisible = secVis;
	w->visNeigh = neigh;
	wlog("world: retail visibility from %s -- %u drivable sections (%u outlined), "
	     "%u visible entries in the index, %u not (texture-only)",
	     bunPath, nDrive, outlined, nList, unknown);
	wlog("world: %u split-section part(s) on their sections' lists, %u touching "
	     "drivable pairs", (unsigned)linked, (unsigned)nNeigh);
	return true;
}

static bool visInside(const World* w, const VisRegion* g, float x, float y) {
	if (!g->numPoints || x < g->bbmin[0] || x > g->bbmax[0] ||
	    y < g->bbmin[1] || y > g->bbmax[1])
		return false;
	const float* p = w->visPts + 2 * g->firstPoint;
	bool in = false;
	for (u32 i = 0, j = g->numPoints - 1u; i < g->numPoints; j = i++) {
		const float xi = p[2 * i], yi = p[2 * i + 1];
		const float xj = p[2 * j], yj = p[2 * j + 1];
		if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
			in = !in;
	}
	return in;
}

// Which drivable section `pos` is in. The current one is tested first, since
// that is the answer on almost every frame. Outside every outline -- airborne
// over a gap, or briefly off the edge of the mapped road -- the last answer
// stands, which is what keeps the city from blinking out mid-jump.
static void visUpdate(World* w, const float pos[3]) {
	if (!w->vis)
		return;
	if (w->visCurrent >= 0 && visInside(w, &w->vis[w->visCurrent], pos[0], pos[1]))
		return;
	int hit = -1;
	for (int i = 0; i < w->visCount; i++) {
		if (i != w->visCurrent && visInside(w, &w->vis[i], pos[0], pos[1])) {
			hit = i;
			break;
		}
	}
	if (hit < 0)
		return;
	w->visCurrent = hit;
	const VisRegion* g = &w->vis[hit];
	memset(w->secVisible, 0, (size_t)w->count);
	for (u32 k = 0; k < g->numVisible; k++)
		w->secVisible[w->visList[g->firstVisible + k]] = 1;
	const u16 n = g->number;
	wlog("world: in drivable section %c%02u -- %u sections visible",
		'A' + n / 100 - 1, n % 100, g->numVisible);
}

// Section i is resident but not on the retail list for where the car is, so
// retail would not have it streamed in. False before the first lookup has an
// answer and whenever L4RA.BUN was not found: then everything resident counts.
static bool visHidden(const World* w, int i) {
	return w->visCurrent >= 0 && w->secVisible && !w->secVisible[i];
}

// The pager falling behind the car, measured (#270). A hole is any stretch of
// frames in which a section on the current visible list, inside HOLE_RADIUS of
// the car, has no model: that is road or building the player can be looking
// at and see nothing where it should be. It is logged when it opens, with the
// speed and what the loader was busy with, and again when it closes, with how
// long it lasted -- which is the number the streaming work has to get to zero.
#define HOLE_RADIUS 150.0f

static void holeTrack(World* w, const float pos[3]) {
	const u64 now = svcGetSystemTick();
	if (w->lastTick) {
		const float dx = pos[0] - w->lastPos[0], dy = pos[1] - w->lastPos[1];
		const float d = sqrtf(dx * dx + dy * dy);
		const float dt = (float)(now - w->lastTick) / (float)SYSCLOCK_ARM11;
		// A teleport is not a speed.
		if (dt > 0.0f && d < 100.0f)
			w->speed += (d / dt - w->speed) * 0.2f;
	}
	memcpy(w->lastPos, pos, sizeof(w->lastPos));
	w->lastTick = now;
	if (w->holeMute || w->visCurrent < 0 || !w->secVisible)
		return;

	int missing = 0, nearest = -1;
	float nearestD = HOLE_RADIUS * HOLE_RADIUS;
	for (int i = 0; i < w->count; i++) {
		const WorldSection* s = &w->secs[i];
		// A section that failed is the worst hole of all, not none: it is
		// the one that never closes.
		if (!w->secVisible[i] || s->model)
			continue;
		const float d = distSqToBox(s, pos);
		if (d > HOLE_RADIUS * HOLE_RADIUS)
			continue;
		missing++;
		if (d <= nearestD) {
			nearestD = d;
			nearest = i;
		}
	}
	if (missing) {
		const float m = sqrtf(nearestD);
		if (!w->holeStart) {
			const Loader* L = &w->ld;
			w->holeStart = now;
			w->holeWorst = 0;
			w->holeNearest = m;
			snprintf(w->holeName, sizeof(w->holeName), "%s", w->secs[nearest].name);
			const WorldSection* hs = &w->secs[nearest];
			wlog("world: hole -- %s visible %.0f m away and not loaded (%d missing), "
				"%.0f km/h, loader %s %s, it is %s", hs->name, (double)m, missing,
				(double)(w->speed * 3.6f),
				L->busy || L->binding ? "on" : "idle",
				L->busy || L->binding ? w->secs[L->sec].name : "",
				hs->failed ? "FAILED" : hs->loading ? "loading"
					: hs->retryFrame ? "retrying" : "queued");
		}
		if (missing > w->holeWorst)
			w->holeWorst = missing;
		if (m < w->holeNearest) {
			w->holeNearest = m;
			snprintf(w->holeName, sizeof(w->holeName), "%s", w->secs[nearest].name);
		}
	} else if (w->holeStart) {
		const u64 t = now - w->holeStart;
		w->holeCount++;
		w->holeTicks += t;
		wlog("world: hole closed after %.0f ms -- worst %d missing, nearest %.0f m (%s); "
			"%u hole(s), %.1f s in all", (double)t * 1000.0 / (double)SYSCLOCK_ARM11,
			w->holeWorst, (double)w->holeNearest, w->holeName, (unsigned)w->holeCount,
			(double)w->holeTicks / (double)SYSCLOCK_ARM11);
		w->holeStart = 0;
	}
}

// ---- The streaming order (TASKS.md #270 P1) ---------------------------------
//
// See STREAM_* at the top and docs/retail_trackstreamer.md. Without visible
// lists -- no L4RA.BUN -- or before the car has been found inside a drivable
// section, the radius pager in world_update() runs instead, as it always did.

// The drivable section a point is in, or -1. `hint` is tried first: a point
// that moves a few metres a frame is nearly always still where it was.
static int visFind(const World* w, float x, float y, int hint) {
	if (hint >= 0 && visInside(w, &w->vis[hint], x, y))
		return hint;
	for (int i = 0; i < w->visCount; i++)
		if (i != hint && visInside(w, &w->vis[i], x, y))
			return i;
	return -1;
}

int world_visible_loaded(const World* w, const float eye[3], u16* out, int max) {
	if (!w || !w->vis || !eye || !out)
		return 0;
	// PORT: retail draws none of it while FindDrivableSection finds nothing,
	// which the eye's outline gaps would turn into every lamp in the street
	// blinking off for a frame as the camera swings over a kerb; the car's
	// own drivable section stands in, as it does for the world's sections.
	int r = visFind(w, eye[0], eye[1], w->visCurrent);
	if (r < 0)
		r = w->visCurrent;
	if (r < 0)
		return 0;
	const VisRegion* g = &w->vis[r];
	int n = 0;
	for (u32 k = 0; k < g->numVisible && n < max; k++) {
		const WorldSection* s = &w->secs[w->visList[g->firstVisible + k]];
		// A split section's later parts share its number and come in with it.
		if (!s->model || strchr(s->name, '.'))
			continue;
		const int num = sectionNumber(s->name);
		if (num >= 0)
			out[n++] = (u16)num;
	}
	return n;
}

// AddCurrentStreamingSections (0x80205CE4): every section on `region`'s list
// is wanted at `tier`, and a wanted one is stamped with this frame. PORT: a
// listed section past DRAW_RADIUS is only read ahead. Retail loads the whole
// list; the port draws nothing that far out but the far scenery, so loading
// it as wanted would hold a loading screen up, or evict something on screen,
// for geometry the fog has already turned to sky.
static void streamMark(World* w, int region, u8 tier, const float pos[3]) {
	const VisRegion* g = &w->vis[region];
	for (u32 k = 0; k < g->numVisible; k++) {
		WorldSection* s = &w->secs[w->visList[g->firstVisible + k]];
		u8 t = tier;
		if (t == STREAM_WANTED && !(s->flags & SECTION_FAR) &&
		    distSqToDrawBox(s, pos) > DRAW_RADIUS * DRAW_RADIUS)
			t = STREAM_PREFETCH;
		if (t == STREAM_WANTED)
			s->lastWanted = w->frame;
		if (s->tier < t)
			s->tier = t;
	}
}

static void streamMarkNeighbours(World* w, int region, const float pos[3]) {
	const VisRegion* g = &w->vis[region];
	for (u32 k = 0; k < g->numNeigh; k++)
		streamMark(w, w->visNeigh[g->firstNeigh + k], STREAM_PREFETCH, pos);
}

// DetermineStreamingSections (0x80205DF8), rebuilt every frame rather than on
// a zone switch: the stamps are the LRU clock, and redoing the marks is a few
// hundred byte writes. False when there is nothing to stream by -- no visible
// lists, no drivable section found yet, or the car put down far from the last
// one -- and the radius pager is to stand in.
static bool streamTiers(World* w, const float pos[3], const float vel[3]) {
	w->frame++;
	w->vel[0] = vel ? vel[0] : 0.0f;
	w->vel[1] = vel ? vel[1] : 0.0f;
	if (!w->vis || w->visCurrent < 0)
		return false;
	const VisRegion* cur = &w->vis[w->visCurrent];
	const float lo[3] = { cur->bbmin[0], cur->bbmin[1], 0.0f };
	const float hi[3] = { cur->bbmax[0], cur->bbmax[1], 0.0f };
	if (distSqToBoxXY(lo, hi, pos) > STREAM_STALE_M * STREAM_STALE_M)
		return false;

	// Far scenery is pinned once loaded, as it always was, so it is always
	// worth reading: about 3 MB for all of it. The world animations' models
	// (#239) are wanted outright: retail's are in the track's global bundle,
	// there from the first frame, and as read-ahead WANIM's box -- the dummy
	// spots the models are baked at -- would rank it by a meaningless
	// distance, last of all from the south of the city.
	for (int i = 0; i < w->count; i++) {
		WorldSection* s = &w->secs[i];
		s->tier = (s->flags & SECTION_ANIM) ? STREAM_WANTED
			: (s->flags & SECTION_FAR) ? STREAM_PREFETCH : STREAM_NONE;
		if (s->flags & SECTION_ANIM)
			s->lastWanted = w->frame;
	}

	const float vx = w->vel[0], vy = w->vel[1];
	const float speed = sqrtf(vx * vx + vy * vy);
	// GetPredictedZone. PORT: retail predicts only inside TrackPathManager
	// zones of type 12 (TPZ_STREAMER_PREDICTION, trackpath.h), and takes the
	// point only when its drivable section is one of that zone's own (up to
	// four) and the line to it crosses none of the streamer's barrier lines.
	// Neither field is decoded, so the port predicts everywhere -- which can
	// only add to what is loaded, never take from it.
	int pred = -1;
	if (speed > 0.0f && speed <= STREAM_PREDICT_TOP) {
		const float t = speed * STREAM_PREDICT_S > STREAM_PREDICT_CAP
			? STREAM_PREDICT_CAP / speed : STREAM_PREDICT_S;
		pred = visFind(w, pos[0] + vx * t, pos[1] + vy * t,
			w->visPred >= 0 ? w->visPred : w->visCurrent);
	}
	w->visPred = pred;
	int ahead = -1;
	if (speed >= 1.0f) {
		float d = speed * STREAM_AHEAD_S;
		if (d > STREAM_AHEAD_CAP)
			d = STREAM_AHEAD_CAP;
		ahead = visFind(w, pos[0] + vx / speed * d, pos[1] + vy / speed * d,
			w->visAhead >= 0 ? w->visAhead : w->visCurrent);
	}
	w->visAhead = ahead;

	streamMark(w, w->visCurrent, STREAM_WANTED, pos);
	if (pred >= 0 && pred != w->visCurrent)
		streamMark(w, pred, STREAM_WANTED, pos);
	if (ahead >= 0)
		streamMark(w, ahead, STREAM_PREFETCH, pos);
	streamMarkNeighbours(w, w->visCurrent, pos);
	if (pred >= 0 && pred != w->visCurrent)
		streamMarkNeighbours(w, pred, pos);
	return true;
}

// GetLoadingPriority (0x80206E74): the distance from the point 1 s ahead to
// the section, discounted by up to STREAM_PRIO_DISCOUNT when the section lies
// ahead and the car is fast, and that distance's 75 m bucket. Slower than
// 1 m/s every section is bucket 0. PORT: retail measures to the section's
// boundary outline and its centre; the port keeps outlines for the drivable
// sections only, so it measures to the core box and the core box's centre.
static float streamRank(const World* w, const WorldSection* s, const float pos[3],
		int* bucket) {
	const float vx = w->vel[0], vy = w->vel[1];
	const float speed = sqrtf(vx * vx + vy * vy);
	const float p[3] = { pos[0] + vx * STREAM_PRIO_AHEAD_S,
		pos[1] + vy * STREAM_PRIO_AHEAD_S, pos[2] };
	float d = sqrtf(distSqToBox(s, p));
	if (d > STREAM_PRIO_CAP)
		d = STREAM_PRIO_CAP;
	float k = 1.0f;
	if (speed >= 1.0f) {
		const float f = speed >= STREAM_PRIO_SPEED ? 1.0f : speed / STREAM_PRIO_SPEED;
		const float cx = 0.5f * (s->coremin[0] + s->coremax[0]) - p[0];
		const float cy = 0.5f * (s->coremin[1] + s->coremax[1]) - p[1];
		const float cl = sqrtf(cx * cx + cy * cy);
		float a = STREAM_PRIO_ANG_HI;
		if (cl > 0.0f) {
			float c = (cx * vx + cy * vy) / (cl * speed);
			if (c > 1.0f)
				c = 1.0f;
			else if (c < -1.0f)
				c = -1.0f;
			a = acosf(c) * (180.0f / (float)M_PI);
		}
		if (a < STREAM_PRIO_ANG_LO)
			a = STREAM_PRIO_ANG_LO;
		else if (a > STREAM_PRIO_ANG_HI)
			a = STREAM_PRIO_ANG_HI;
		k = 1.0f - (STREAM_PRIO_ANG_HI - a) / STREAM_PRIO_ANG_SPAN * f * STREAM_PRIO_DISCOUNT;
	}
	const float r = d * k;
	if (bucket) {
		int b = speed < 1.0f ? 0 : (int)(r / STREAM_PRIO_BUCKET);
		*bucket = b < 0 ? 0 : b > 2 ? 2 : b;
	}
	return r;
}

// AssignLoadingPriority (0x8020714C) and StartLoadingSections (0x80207208):
// the wanted section with the lowest bucket x 100000 + tie-break goes first.
// PORT: retail's tie-break (+0x20) is not decoded; the discounted distance
// stands in. Read-ahead sections come after every wanted one, nearest first,
// and not at all while world_prime() holds the loading screen up.
static int streamPick(const World* w, const float pos[3]) {
	int best = -1;
	float bestKey = 0.0f;
	for (int i = 0; i < w->count; i++) {
		const WorldSection* s = &w->secs[i];
		if (s->tier == STREAM_NONE || s->model || s->failed || s->loading)
			continue;
		if (w->priming && s->tier != STREAM_WANTED)
			continue;
		if (s->retryFrame && (s32)(w->frame - s->retryFrame) < 0)
			continue;
		int b;
		const float r = streamRank(w, s, pos, &b);
		const float key = s->tier == STREAM_WANTED ? (float)b * 100000.0f + r
			: 300000.0f + r;
		if (best < 0 || key < bestKey) {
			best = i;
			bestKey = key;
		}
	}
	return best;
}

static u32 streamCeiling(const World* w) {
	return (u32)((u64)w->budget * STREAM_KEEP_PCT / 100);
}

// UnloadLeastRecentlyUsedSection (0x80205068): the resident section nobody
// wants that was wanted longest ago. For a wanted load with none of those
// left, the read-ahead section farthest away goes next. Far scenery and wanted
// sections are never chosen; -1 when nothing can go.
static int streamVictim(const World* w, bool forWanted, const float pos[3]) {
	int v = -1;
	u32 oldest = 0;
	for (int i = 0; i < w->count; i++) {
		const WorldSection* s = &w->secs[i];
		if (!s->model || (s->flags & SECTION_FAR) || s->tier != STREAM_NONE)
			continue;
		if (v < 0 || s->lastWanted < oldest) {
			v = i;
			oldest = s->lastWanted;
		}
	}
	if (v >= 0 || !forWanted)
		return v;
	float worst = -1.0f;
	for (int i = 0; i < w->count; i++) {
		const WorldSection* s = &w->secs[i];
		if (!s->model || (s->flags & SECTION_FAR) || s->tier != STREAM_PREFETCH)
			continue;
		const float d = distSqToBox(s, pos);
		if (d > worst) {
			worst = d;
			v = i;
		}
	}
	return v;
}

// A load ran out of memory with the world inside its budget: the heap the
// budget was sized from at world_open() has less in it now (a race's opponent
// cars, its HUD), or is too broken up to give one pool the room the totals
// promise. Either way what the world holds now is what memory turned out to
// allow, so the budget comes down to just under it -- the wanted retry then
// evicts what nobody wants to make room, and read-ahead stops short of the
// line. Never below half of what world_open() sized, and world_retarget()
// puts it back for the next place, whose heap is a different one.
static void streamShrink(World* w, const char* why) {
	const u32 held = w->residentBytes + w->bankBytes;
	u32 b = held - held / 16;
	if (b < w->budgetMax / 2)
		b = w->budgetMax / 2;
	if (b >= w->budget)
		return;
	wlog("world: budget %u -> %u KB, memory ran out at %u KB loading %s",
		w->budget >> 10, b >> 10, held >> 10, why);
	w->budget = b;
	w->streamFull = false;
}

// MakeSpaceInPool (0x802065E4), on the budget rather than on the largest free
// block: room for section `best`, made by evicting. A wanted section may fill
// the budget; a read-ahead one only STREAM_KEEP_PCT of it, and only by
// evicting what nobody wants. False, with nothing loaded, when there is no
// room to be had.
static bool streamMakeRoom(World* w, int best, const float pos[3], bool* changed) {
	const WorldSection* c = &w->secs[best];
	const bool wanted = c->tier == STREAM_WANTED;
	const u32 limit = wanted ? w->budget : streamCeiling(w);
	while (w->residentBytes + w->bankBytes + c->memBytes > limit) {
		const int v = streamVictim(w, wanted, pos);
		if (v < 0) {
			if (wanted && !w->streamFull) {
				w->streamFull = true;
				wlog("world: %s is wanted and does not fit -- %u+%u KB of %u KB, all "
					"of it wanted", c->name, w->residentBytes >> 10, w->bankBytes >> 10,
					w->budget >> 10);
			}
			return false;
		}
		dropSection(w, v);
		*changed = true;
	}
	return true;
}

// A resident section's empty texture slot (s->texMissing), filled once there
// is memory for it: one texture a frame, between loads, read on this thread
// the way bankAcquire() always could. The model holds the slot array itself,
// so filling a slot is all it takes for the next frame to draw it.
static bool texBackfill(World* w) {
	for (int i = 0; i < w->count; i++) {
		WorldSection* s = &w->secs[i];
		if (!s->texMissing || !s->model || !s->texSlots)
			continue;
		const int n = model3d_texture_count(s->model);
		for (int k = 0; k < n; k++) {
			if (s->texSlots[k])
				continue;
			const u32 h = model3d_texture_hash(s->model, k);
			BankTexture* t = h ? bankFind(w, h) : NULL;
			if (!t || t->failed)
				continue;
			C3D_Tex* tex = bankAcquire(w, h, NULL);
			if (!tex)
				return false;   // still no room; the loads go on meanwhile
			s->texSlots[k] = tex;
			if (--s->texMissing == 0)
				wlog("world: %s has all its textures now", s->name);
			return true;
		}
		s->texMissing = 0;
	}
	return false;
}

// ------------------------------------------------- arena compaction (#270)
//
// The arena breaks up however its two ends are kept apart: a race's sections
// come and go in every size between 50 KB and 2.3 MB, and the textures with
// them. On 2026-10-25 it held 12 MB free with no block over 1 MB, and C22 and
// B26, wanted, would not fit. So blocks move (linarena.h): a resident
// section's pools and a resident texture can each be copied somewhere else
// between frames and repointed, which closes the gaps between them.
//
// What may move: a section's pools once it is published (s->model) -- the one
// in flight is the loader's, its pools filled and walked by the worker -- and
// a texture once it is valid; a pending one is the worker's to read into.

// Bytes compaction may copy in one world_update(): for a wanted section that
// does not fit, for a read-ahead one, and for keeping the run free between
// loads. memcpy over the linear heap is in the hundreds of MB/s on the
// console, so the first is a few ms, once, instead of a hole.
#define COMPACT_WANTED_BYTES (2048u * 1024)
#define COMPACT_AHEAD_BYTES  (512u * 1024)
#define COMPACT_IDLE_BYTES   (256u * 1024)
// streamTidy() rests this many frames after finding nothing it can move.
#define TIDY_REST_FRAMES     60

static bool arenaCanMove(u32 tag, const void* p, void* user) {
	World* w = (World*)user;
	const u32 i = tag & ~TAG_KIND;
	if ((tag & TAG_KIND) == TAG_TEXTURE) {
		if (i >= (u32)w->bankCount)
			return false;
		const BankTexture* t = &w->bank[i];
		return t->valid && !t->pending && t->tex.data == p;
	}
	if ((tag & TAG_KIND) == TAG_SECTION) {
		if (i >= (u32)w->count || !w->secs[i].model)
			return false;
		Model3D* m = w->secs[i].model;
		return p == model3d_vertex_dest(m) || p == model3d_index_dest(m);
	}
	return false;
}

static void arenaMoved(u32 tag, void* from, void* to, void* user) {
	World* w = (World*)user;
	const u32 i = tag & ~TAG_KIND;
	if ((tag & TAG_KIND) == TAG_TEXTURE)
		w->bank[i].tex.data = to;
	else
		model3d_relocate(w->secs[i].model, from, to);
}

static ArenaRun streamCompact(World* w, u32 need, u32 maxBytes, u32* moved) {
	const ArenaMover mv = { arenaCanMove, arenaMoved, w };
	const ArenaRun r = arena_make_run(need, maxBytes, &mv, moved);
	if (*moved) {
		w->compactBytes += *moved;
		w->compactSteps++;
	}
	return r;
}

// Between loads, the run put back before anything needs it: one step of at
// most COMPACT_IDLE_BYTES a frame while the arena's largest free block is
// short of runTarget and there is enough free in all to make one.
static void streamTidy(World* w) {
	if (!arena_active() || !w->runTarget || (s32)(w->frame - w->tidyWait) < 0)
		return;
	if (arena_largest() >= w->runTarget) {
		if (w->tidySteps) {
			wlog("world: tidy -- %u KB free in one run again, %u KB moved in %u step(s), "
				"%u ms", (unsigned)(arena_largest() >> 10), w->tidyBytes >> 10, w->tidySteps,
				(unsigned)(w->tidyTicks * 1000 / SYSCLOCK_ARM11));
			w->tidyBytes = w->tidySteps = 0;
			w->tidyTicks = 0;
		}
		return;
	}
	if (arena_size() - arena_used() < w->runTarget + w->runTarget / 4)
		return;   // too little free for a run worth making; eviction's job
	const u64 t0 = svcGetSystemTick();
	u32 moved = 0;
	const ArenaRun r = streamCompact(w, w->runTarget, COMPACT_IDLE_BYTES, &moved);
	w->tidyTicks += svcGetSystemTick() - t0;
	if (moved) {
		w->tidyBytes += moved;
		w->tidySteps++;
	}
	if (r == ARENA_RUN_NONE)
		w->tidyWait = w->frame + TIDY_REST_FRAMES;
}

// MakeSpaceInPool's other half: the budget says there is room, and the arena
// has to agree that the section's two pools will each find a block. When they
// will not: an unwanted section whose eviction alone opens the room goes;
// failing that, blocks move until there is a run for both pools (retail's
// hole filling); failing that -- not enough free in all -- the section whose
// eviction opens the longest run goes, among those streamVictim() would
// allow. `mayEvict` is false for the radius pager, whose sections carry no
// tiers to judge by. False, with nothing loaded, when this frame makes no
// room, which it may still be making.
static bool streamMakeFit(World* w, int best, bool mayEvict, bool* changed) {
	if (!arena_active())
		return true;
	const WorldSection* c = &w->secs[best];
	const bool wanted = c->tier == STREAM_WANTED;
	const u32 vb = c->numVerts * MODEL3D_VERTEX_BYTES, ib = c->numIdx * 2u;
	if (!vb || !ib)
		return true;
	const u32 need = vb > ib ? vb : ib;
	while (!arena_fits2(vb, ib)) {
		int v = -1;
		u32 vRun = 0, vAge = 0;
		bool vEnough = false;
		if (mayEvict) {
			for (int i = 0; i < w->count; i++) {
				const WorldSection* s = &w->secs[i];
				if (!s->model || (s->flags & SECTION_FAR) || s->tier != STREAM_NONE)
					continue;
				if (arena_run_if_freed(model3d_vertex_dest(s->model),
				        model3d_index_dest(s->model)) >= need &&
				    (v < 0 || s->lastWanted < vAge)) {
					v = i;
					vAge = s->lastWanted;
				}
			}
			if (v >= 0) {
				dropSection(w, v);
				*changed = true;
				continue;
			}
		}

		const u64 t0 = svcGetSystemTick();
		u32 moved = 0;
		// Behind a loading screen there is no frame to keep: all of it now,
		// or world_prime() would see nothing submitted and stop paging in.
		const ArenaRun r = streamCompact(w, c->memBytes, w->priming ? 0xffffffffu :
			wanted ? COMPACT_WANTED_BYTES : COMPACT_AHEAD_BYTES, &moved);
		if (moved)
			wlog("world: compact for %s -- %u KB moved, %u us, %u KB free in one run, %u of "
				"%u KB used%s", c->name, moved >> 10,
				(unsigned)((svcGetSystemTick() - t0) * 1000000 / SYSCLOCK_ARM11),
				(unsigned)(arena_largest() >> 10), (unsigned)(arena_used() >> 10),
				(unsigned)(arena_size() >> 10), r == ARENA_RUN_MOVING ? ", more next frame" : "");
		if (r == ARENA_RUN_MOVING)
			return false;
		if (r == ARENA_RUN_READY && arena_fits2(vb, ib))
			return true;

		if (mayEvict) {
			for (int pass = 0; pass < 2 && v < 0; pass++) {
				if (pass && !wanted)
					break;
				const u8 tier = pass ? STREAM_PREFETCH : STREAM_NONE;
				for (int i = 0; i < w->count; i++) {
					const WorldSection* s = &w->secs[i];
					if (!s->model || (s->flags & SECTION_FAR) || s->tier != tier)
						continue;
					const u32 run = arena_run_if_freed(model3d_vertex_dest(s->model),
						model3d_index_dest(s->model));
					const bool enough = run >= need;
					const bool better = v < 0 || (enough && !vEnough) ||
						(enough == vEnough && (enough ? s->lastWanted < vAge : run > vRun));
					if (better) {
						v = i;
						vRun = run;
						vAge = s->lastWanted;
						vEnough = enough;
					}
				}
			}
		}
		if (v < 0) {
			if (wanted && !w->streamFull) {
				w->streamFull = true;
				wlog("world: %s is wanted and its pools (%u + %u KB) do not fit -- arena "
					"%u of %u KB used, %u KB largest, nothing left to evict or move", c->name,
					vb >> 10, ib >> 10, (unsigned)(arena_used() >> 10),
					(unsigned)(arena_size() >> 10), (unsigned)(arena_largest() >> 10));
			}
			return false;
		}
		dropSection(w, v);
		*changed = true;
	}
	return true;
}

// A header the worker read ahead comes back in well under a millisecond, and
// waiting that long for it here takes the section on to its geometry and
// textures this frame instead of the next (#270 P6). Driving, every load
// waited a frame at each of its three hand-offs: 40 ms of a median 100 ms.
#define LOADER_QUICK_POLLS 4
#define LOADER_QUICK_NS    100000LL   // 0.1 ms
static bool loaderQuick(World* w) {
	Loader* L = &w->ld;
	if (!L->thread || w->priming || !L->hinted)
		return false;
	for (int k = 0; k < LOADER_QUICK_POLLS && L->busy; k++) {
		svcSleepThread(LOADER_QUICK_NS);
		if (LightEvent_TryWait(&L->done)) {
			L->busy = false;
			return advanceLoad(w);
		}
	}
	return false;
}

bool world_streaming(const World* w) {
	return w && (w->ld.busy || w->ld.binding);
}

bool world_update(World* w, const float pos[3], const float vel[3]) {
	if (!w || !w->count)
		return false;

	visUpdate(w, pos);
	holeTrack(w, pos);
	const bool retail = streamTiers(w, pos, vel);
	w->retailPick = retail;

	bool changed = false;

	if (retail) {
		// Back down to STREAM_KEEP_PCT, one unwanted section a frame, the
		// longest unwanted first. Wanted loads may have taken it past that.
		if (w->residentBytes + w->bankBytes > streamCeiling(w)) {
			const int v = streamVictim(w, false, pos);
			if (v >= 0) {
				dropSection(w, v);
				changed = true;
			}
		}
	} else {
		// Drop first, so the budget check below sees the space this frame's
		// departures freed. A section that has gone past DROP_RADIUS is gone
		// even if nothing wants its memory: keeping it costs a per-frame
		// draw-list walk for geometry that is half a kilometre away.
		for (int i = 0; i < w->count; i++) {
			if (!w->secs[i].model || (w->secs[i].flags & SECTION_FAR))
				continue;   // far scenery is pinned once loaded
			if (distSqToBox(&w->secs[i], pos) > DROP_RADIUS * DROP_RADIUS) {
				dropSection(w, i);
				changed = true;
			}
		}
	}

	// A request in flight is either still reading -- in which case this frame
	// does nothing more, which is the whole point of #176 -- or it has just
	// come back and gets taken one step forward.
	Loader* L = &w->ld;
	if (L->busy) {
		if (!LightEvent_TryWait(&L->done)) {
			// The main thread is free while the worker reads: put the free
			// run back now, away from the in-flight section's blocks.
			streamTidy(w);
			return changed;
		}
		L->busy = false;
		if (advanceLoad(w))
			changed = true;
		return changed;
	}
	// A section part-way through binding owes this frame its next few slots
	// before any new request is considered -- see commitSection().
	if (L->binding)
		return commitSection(w) || changed;
	if (texBackfill(w))
		return true;

	if (retail) {
		const int next = streamPick(w, pos);
		if (next < 0)
			streamTidy(w);
		else if (streamMakeRoom(w, next, pos, &changed) &&
		         streamMakeFit(w, next, true, &changed)) {
			submitSection(w, next);
			if (loaderQuick(w))
				changed = true;
		}
		return changed;
	}

	// The radius pager: the nearest missing section inside LOAD_RADIUS, one
	// per call.
	int best = -1;
	float bestD = LOAD_RADIUS * LOAD_RADIUS;
	for (int i = 0; i < w->count; i++) {
		WorldSection* s = &w->secs[i];
		if (s->model || s->failed || s->loading)
			continue;
		if (s->retryFrame && (s32)(w->frame - s->retryFrame) < 0)
			continue;
		float d = distSqToBox(s, pos);
		if (d <= bestD) {
			bestD = d;
			best = i;
		}
	}
	if (best < 0) {
		streamTidy(w);
		return changed;
	}

	// Budget. When the candidate does not fit, the farthest resident section
	// makes way for it -- but only if it really is farther, otherwise a full
	// world would evict the ground under the car to load a backdrop.
	// The bank's uploaded pixels are linear memory the sections caused to be
	// spent, so they belong in the same budget the section blobs are checked
	// against -- otherwise a world whose geometry is small but whose art is
	// not would sail past the cap.
	while ((!(w->secs[best].flags & SECTION_FAR) &&
	        w->residentNear >= WORLD_MAX_RESIDENT) ||
	       w->residentBytes + w->bankBytes + w->secs[best].memBytes > w->budget) {
		int worst = -1;
		float worstD = bestD * EVICT_MARGIN;
		for (int i = 0; i < w->count; i++) {
			if (!w->secs[i].model || (w->secs[i].flags & SECTION_FAR))
				continue;
			float d = distSqToBox(&w->secs[i], pos);
			if (d > worstD) {
				worstD = d;
				worst = i;
			}
		}
		if (worst < 0)
			return changed;   // everything resident is closer; leave it alone
		dropSection(w, worst);
		changed = true;
	}
	if (!streamMakeFit(w, best, false, &changed))
		return changed;

	submitSection(w, best);
	return changed;
}

// The blocking form, for entering the world. There is nothing to keep smooth
// behind the loading screen, and a first frame showing a single tile of a city
// looks like a bug -- so here the request is waited out rather than left in
// flight.
//
// What it waits on can be a 300 ms read, and the loading screen it sits behind
// is animated -- so while it waits it draws that screen's next frame every
// 100 ms through the hook main.c sets (#270 P4). A frame of it costs
// about 9 ms on Azahar, and the worker is parked in the read meanwhile.
// While the load pump is armed (loadpump.h, #303) the pump draws instead, as
// often as its spacing allows.
#define PRIME_TICK_TICKS ((u64)SYSCLOCK_ARM11 / 10)   // 100 ms
#define PRIME_POLL_NS    1000000LL                     // 1 ms
static void (*s_primeTick)(void*) = NULL;
static void* s_primeTickUd = NULL;

void world_set_prime_tick(void (*tick)(void* ud), void* ud) {
	s_primeTick = tick;
	s_primeTickUd = ud;
}

bool world_prime(World* w, const float pos[3]) {
	if (!w)
		return false;
	w->holeMute = true;
	w->priming = true;
	w->submitted = false;
	// A section already in flight when the switch began -- free roam's last
	// pick -- can finish inside this one world_update(), which then asks for
	// nothing new. That is not "nothing left": the destination was not looked
	// at yet. On 2026-10-25 it ended the race switch's page-in after C48, and
	// D45, 118 m from the grid, was a hole on the first frame.
	const bool inFlight = w->ld.busy || w->ld.binding;
	const bool changed = world_update(w, pos, NULL);
	w->holeMute = false;
	// Whether there was anything to do: a section asked for here, one already
	// in flight that the loop below sees through, or a texture backfilled or a
	// section dropped, after which the next call may have room to ask.
	const bool more = inFlight || changed || w->submitted || w->ld.busy || w->ld.binding;
	// Behind the loading screen there is no frame to protect, so the binding
	// spread is simply looped out rather than waited on.
	while (w->ld.busy || w->ld.binding) {
		if (w->ld.busy) {
			if (s_primeTick) {
				// Polled rather than LightEvent_WaitTimeout(): on Azahar a
				// timed wait woke 7 ms late per hand-off, 20 ms a section.
				u64 last = svcGetSystemTick();
				while (!LightEvent_TryWait(&w->ld.done)) {
					// Under the load pump its own spacing decides (loadpump.h).
					if (loadpump_armed()) {
						loadpump_poll();
						svcSleepThread(PRIME_POLL_NS);
					} else if (svcGetSystemTick() - last >= PRIME_TICK_TICKS) {
						s_primeTick(s_primeTickUd);
						last = svcGetSystemTick();
					} else {
						svcSleepThread(PRIME_POLL_NS);
					}
				}
			} else {
				LightEvent_Wait(&w->ld.done);
			}
			w->ld.busy = false;
			advanceLoad(w);
		} else {
			commitSection(w);
		}
	}
	// Not before: the read-ahead hint the loop above leaves is a wanted
	// section too, as the next call will ask for.
	w->priming = false;
	// Nothing left to ask for should mean nothing wanted is missing: a
	// wanted section left out here is a hole on the first frame (#270).
	if (!more) {
		int n = 0;
		for (int i = 0; i < w->count; i++) {
			const WorldSection* s = &w->secs[i];
			if (s->tier != STREAM_WANTED || s->model || (s->flags & SECTION_FAR))
				continue;
			if (n++ < 4)
				wlog("world: prime done with %s wanted and not loaded -- %s%s%s%s, %.0f m",
					s->name, s->loading ? "loading " : "", s->failed ? "failed " : "",
					s->retryFrame ? "retry " : "", w->ld.busy ? "loader busy" : "loader idle",
					(double)sqrtf(distSqToBox(s, pos)));
		}
	}
	return more;
}

void world_retarget(World* w, const float pos[3]) {
	if (!w)
		return;
	visUpdate(w, pos);
	const u32 before = w->residentBytes + w->bankBytes;
	int dropped = 0;
	// A new place, and a heap the old one's shortage says nothing about.
	if (w->budget != w->budgetMax) {
		wlog("world: budget %u -> %u KB for the destination", w->budget >> 10,
			w->budgetMax >> 10);
		w->budget = w->budgetMax;
	}
	for (int i = 0; i < w->count; i++)
		w->secs[i].retryFrame = 0;
	const bool retail = streamTiers(w, pos, NULL);
	for (int i = 0; i < w->count; i++) {
		const WorldSection* s = &w->secs[i];
		if (!s->model || (s->flags & SECTION_FAR))
			continue;
		if (retail ? s->tier == STREAM_NONE
		           : distSqToBox(s, pos) > DROP_RADIUS * DROP_RADIUS) {
			dropSection(w, i);
			dropped++;
		}
	}
	wlog("world: retarget -- %d section(s) dropped, %u KB, %d kept for the destination",
		dropped, (before - (w->residentBytes + w->bankBytes)) >> 10, w->resident);
	if (arena_active())
		wlog("world: compaction so far %u KB in %u step(s); arena %u of %u KB used, "
			"%u KB largest", w->compactBytes >> 10, w->compactSteps,
			(unsigned)(arena_used() >> 10), (unsigned)(arena_size() >> 10),
			(unsigned)(arena_largest() >> 10));
}

// ------------------------------------------------------------- the frustum
//
// Until now drawing culled by distance alone, which on a 520 m radius means
// every resident section around the car -- including the ten or so directly
// behind the camera, and every far-scenery shell in the index unconditionally.
// That is most of a frame's triangles submitted for nothing, and this GPU was
// fill-bound on it: the frame's own submission cost measured 12 ms while
// C3D_FrameEnd returned immediately and C3D_FrameBegin blocked for the rest of
// a 33 ms frame, which is what being a frame behind the GPU looks like.
//
// The four side planes come straight out of the view-projection matrix: a point
// is inside the clip volume when x+w, w-x, y+w and w-y are all >= 0, and each
// of those is a row sum of the matrix. Near and far are left to the existing
// distance test, which is what actually bounds the draw radius. The screen's
// 90-degree rotation is already in the matrix, so nothing here has to know
// about it.
typedef struct {
	float p[4][4];
} Frustum;

static void frustumFromVp(const C3D_Mtx* vp, Frustum* f) {
	const C3D_FVec* r = vp->r;
	for (int k = 0; k < 4; k++) {
		const C3D_FVec* row = (k < 2) ? &r[0] : &r[1];
		const float s = (k & 1) ? -1.0f : 1.0f;
		f->p[k][0] = r[3].x + s * row->x;
		f->p[k][1] = r[3].y + s * row->y;
		f->p[k][2] = r[3].z + s * row->z;
		f->p[k][3] = r[3].w + s * row->w;
	}
}

// Conservative: rejects only when the box's most-positive corner is still on
// the wrong side of a plane, so a box that straddles the edge is kept.
static bool aabbOutside(const Frustum* f, const float lo[3], const float hi[3]) {
	for (int k = 0; k < 4; k++) {
		const float* p = f->p[k];
		const float x = (p[0] >= 0.0f) ? hi[0] : lo[0];
		const float y = (p[1] >= 0.0f) ? hi[1] : lo[1];
		const float z = (p[2] >= 0.0f) ? hi[2] : lo[2];
		if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f)
			return true;
	}
	return false;
}

// How many sections the last world_draw() actually submitted, for the in-game
// status overlay -- the resident count alone says nothing about frame cost.
static int s_drawnSections = 0;
// Scenery instances drawn at their A and at their B model (#247).
static u32 s_drawnLodA = 0;
static u32 s_drawnLodB = 0;
// The view the instances' levels are picked from: world_set_lod_view().
static bool  s_lodViewSet = false;
static float s_lodViewEye[3];
static float s_lodViewFwd[3];
static float s_lodViewH = 0.0f;
// And what those sections cost once the sub-section cull (#68) has had its say.
// The section count cannot show that any more: a section cut down to two of its
// cells and one drawn whole both count as one section, so the only honest
// measure of what the cull did is the calls and triangles that reached the GPU.
// Snapshotted around world_draw()'s own loop rather than read from model3d.c
// directly, so the car, the shadow and the skydome -- drawn outside it, with
// the cull off -- stay out of the world's number.
static u32 s_drawnCalls = 0;
static u32 s_drawnTris = 0;

int world_drawn_count(const World* w) {
	(void)w;
	return s_drawnSections;
}

void world_drawn_lod(const World* w, u32* outA, u32* outB) {
	(void)w;
	if (outA) *outA = s_drawnLodA;
	if (outB) *outB = s_drawnLodB;
}

// CalculateH (Speed.elf 0x801d8bf8): 256 / tan(fov / 2), with the angle in the
// game's 16-bit units and halved as an integer, and 10 if the tangent is 0.
// The GameCube camera hands it +0xc4 (CreateViewMatricies), the same field
// g_chase.fov carries.
void world_set_lod_view(const World* w, const float eye[3], const float fwd[3],
		float fovU16) {
	(void)w;
	if (!eye || !fwd) {
		s_lodViewSet = false;
		return;
	}
	const u16 half = (u16)((u16)(s32)fovU16 >> 1);
	const float t = tanf((float)half * (2.0f * (float)M_PI / 65536.0f));
	s_lodViewH = t != 0.0f ? 256.0f / t : 10.0f;
	memcpy(s_lodViewEye, eye, sizeof(s_lodViewEye));
	memcpy(s_lodViewFwd, fwd, sizeof(s_lodViewFwd));
	s_lodViewSet = true;
}

// CarRenderInfo::Render (Speed.elf 0x80239018) sizes the car with
// eView::GetPixelSize (0x800cd580) at a radius of 3 m, returns before drawing
// anything at 3 pixels or fewer, and otherwise steps down from its minimum
// level while the size is under that level's CarBodyLodSwapSize (0x8033f0a0:
// 120, 45, 20, 0). GetPixelSize: nothing behind the camera by more than the
// radius; past half a radius in front of the sphere, radius * H over the
// distance to it, and 2H closer than that. The wheels' clamp to C is in the
// model (carkit.c), the airborne car's D to C is main.c's carAirLod, and the
// split-screen 0.7 scale has no screen to apply to.
#define CAR_LOD_RADIUS 3.0f
static const int kCarLodSwap[4] = { 120, 45, 20, 0 };
int world_car_pixels(const World* w, const float pos[3]) {
	(void)w;
	if (!s_lodViewSet || !pos)
		return 0;
	const float R = CAR_LOD_RADIUS;
	const float vx = pos[0] - s_lodViewEye[0];
	const float vy = pos[1] - s_lodViewEye[1];
	const float vz = pos[2] - s_lodViewEye[2];
	// fcmpu + bge: a NaN falls through to "behind" as well.
	int P = 0;
	if (vx * s_lodViewFwd[0] + vy * s_lodViewFwd[1] + vz * s_lodViewFwd[2] >= -R) {
		const float d2 = vx * vx + vy * vy + vz * vz;
		const float d = d2 > 5.0e-11f ? sqrtf(d2) : 0.0f;
		const float dr = d - R;
		P = (int)(dr > R * 0.5f ? R * s_lodViewH / dr : s_lodViewH + s_lodViewH);
	}
	return P;
}

int world_car_lod(const World* w, const float pos[3], int minLod) {
	if (!s_lodViewSet || s_dbgNearLod || !pos)
		return minLod;
	const int P = world_car_pixels(w, pos);
	if (P <= 3)
		return -1;
	int lod = minLod;
	while (lod < 3 && P < kCarLodSwap[lod])
		lod++;
	return lod;
}

static const int kTrafficLodSwap[4] = { 45, 20, 8, 0 };
int world_traffic_lod(const World* w, const float pos[3], int minLod) {
	if (!s_lodViewSet || s_dbgNearLod || !pos)
		return minLod;
	const int P = world_car_pixels(w, pos);
	if (P <= 3)
		return -1;
	int lod = minLod;
	while (lod < 3 && P < kTrafficLodSwap[lod])
		lod++;
	return lod;
}

float world_traffic_lod_c_fade(const World* w, const float pos[3]) {
	const float P = (float)world_car_pixels(w, pos);
	return (P - (float)kTrafficLodSwap[2]) / (float)(kTrafficLodSwap[1] - kTrafficLodSwap[2]);
}

float world_car_lod_c_fade(const World* w, const float pos[3]) {
	const float P = (float)world_car_pixels(w, pos);
	return (P - (float)kCarLodSwap[2]) / (float)(kCarLodSwap[1] - kCarLodSwap[2]);
}

void world_drawn_submitted(const World* w, u32* outCalls, u32* outTris) {
	(void)w;
	if (outCalls) *outCalls = s_drawnCalls;
	if (outTris)  *outTris  = s_drawnTris;
}

// ---------------------------------------------------------------- GPU kicks
//
// TASKS.md #68. citro3d does not start the GPU on a frame until C3D_FrameEnd(),
// and C3D_FrameBegin() waits for the GPU to finish before the next frame is
// recorded, since there is one command buffer. So the CPU's build and the
// GPU's draw add up instead of overlapping: in the card's densest views a
// 4-6 ms build, then 13-15 ms of GPU, for 18-21 ms a frame. world_draw()
// hands the GPU what has been recorded so far every KICK_TICKS of recording
// instead. C3D_FrameSplit() closes the list off and queues it, which citro3d's
// own C3D_SyncDisplayTransfer() also does mid-frame, and gxCmdQueueRun()
// starts the queue on it rather than leaving that to C3D_FrameEnd(). The GPU
// then works through the sky and the first sections while the CPU is still
// recording the rest, and the frame costs about the larger of the two rather
// than their sum.
//
// C3D_FrameEnd(0) flushes the whole linear heap out of the CPU's cache before
// it runs the queue, so the first kick does that flush, before the GPU reads
// anything: a section committed this frame, its cull cells' index rewrite, a
// sky swapped in -- nothing written to the heap since the last frame has been
// flushed yet.
//
// C3D_FrameEnd() itself has to find the queue stopped, as it always did.
// It queues the last list, then the display transfers, then sets the flags
// its queue-finished callback reads -- swap the screens, stop the GPU timer
// -- and only then runs the queue. A queue already running starts each entry
// the moment it is queued, and one that runs dry before the flags are set
// finishes the frame without them: the swap waits for the next frame's first
// kick, a frame late. On Azahar, whose GPU is all but instant, that was every
// frame. So world_gpu_frame_end() waits for the kicked lists to finish and
// stops the queue (libctru's gxCmdQueueStop() on an idle queue just
// deactivates it), and C3D_FrameEnd() then queues and runs the rest exactly as
// before. The CPU's wait there is time it would spend in C3D_FrameBegin()'s
// wait anyway, and the heap flush for what was drawn after world_draw()
// (citro2d's vertices, above all) is done first, while the GPU is still busy,
// rather than inside C3D_FrameEnd() while it sits idle.
//
// Between kicks the queue running dry does nothing: the callback's flags are
// not set until C3D_FrameEnd(). The queue holds 32 entries a frame
// (C3D_Init()'s; one more is a svcBreak), and the clears, KICK_MAX kicks, the
// last list and the two display transfers are well inside that.
// #349: the env map kicks after each face and after its fold
// (world_gpu_kick_clean()), so the GPU draws them while the CPU records the
// next face and the road's reflection. KICK_MAX counts those too: three faces
// and the fold on an ordinary frame, hence 12 and not 8. The queue then holds
// at most the clears, 12 kicks, world_draw()'s first kick for each eye, the
// last list and the transfers -- still well inside 32.
// #521 round 3: the road's reflection kicks twice as it is recorded
// (world_gpu_kick_early()), so 14: the queue then holds at most the clears,
// 14 kicks, world_draw()'s first for each eye, the light trails' and the
// smear's copies, the last list and the transfers -- about 24, inside 32.
#define KICK_TICKS (SYSCLOCK_ARM11 / 2000)   // 0.5 ms of recording a list
#define KICK_MAX   14

// citro3d's context (citro3d's internal.h). Its first member is the
// gxCmdQueue_s that C3D_Init() binds to GX, and that member is what citro3d's
// own C3D_FrameBegin() and C3D_SyncDisplayTransfer() pass to libctru's
// gxCmdQueue calls, so it is all a kick needs.
extern gxCmdQueue_s __C3D_Context;
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;

static bool s_gpuKicks = true;
static int  s_kicks;        // this frame's, until world_gpu_frame_end()
static u64  s_kickFirst;    // the first one's tick
static u64  s_kickLast;     // the last one's

// #345: what the whole-heap cache flushes below cost the CPU, read (and
// cleared) by the profiler. Azahar has no data cache to clean, so only a
// console run says anything here.
static u64 s_flushTicks;
static u32 s_flushes;
// #352: the GPU only needs the CPU's dirty lines written back, so the heap is
// cleaned (svcStoreProcessDataCache, straight to the kernel), not cleaned and
// invalidated through GSP's IPC (GSPGPU_FlushDataCache). The invalidate threw
// away every cached line just before the batch walk ran, so the walk started
// cold -- and the walk's tables are not even in the linear heap. Every place
// the CPU reads what the GPU or a DMA wrote invalidates its own range already
// (the shot grabs, the reflection and stereo reads, mvd_player).
// DbgGspFlush=1 goes back to the GSP flush; a refused svc falls back to it too.
static bool s_gspFlush = false;
void world_set_gsp_flush(bool on) {
	s_gspFlush = on;
}
static void heapFlush(void) {
	const u64 t0 = svcGetSystemTick();
	if (s_gspFlush || R_FAILED(svcStoreProcessDataCache(CUR_PROCESS_HANDLE,
			__ctru_linear_heap, __ctru_linear_heap_size))) {
		if (!s_gspFlush) {
			s_gspFlush = true;
			wlog("world: svcStoreProcessDataCache refused, back to GSPGPU_FlushDataCache");
		}
		GSPGPU_FlushDataCache((const void*)__ctru_linear_heap, __ctru_linear_heap_size);
	}
	s_flushTicks += svcGetSystemTick() - t0;
	s_flushes++;
}
void world_flush_stats(u64* ticks, u32* count) {
	*ticks = s_flushTicks;
	*count = s_flushes;
	s_flushTicks = 0;
	s_flushes = 0;
}

static void gpuKick(void) {
	const u64 now = svcGetSystemTick();
	if (!s_kicks) {
		heapFlush();
		s_kickFirst = now;
	}
	C3D_FrameSplit(GX_CMDLIST_FLUSH);
	gpuwatch_mark_queue();
	gxCmdQueueRun(&__C3D_Context);
	s_kicks++;
	s_kickLast = now;
}

void world_set_gpu_kicks(bool on) {
	s_gpuKicks = on;
}

// Unlike world_draw()'s, this one's list can hold draws of vertices the CPU
// wrote after the frame's first kick -- the shadows, the lights, the neon --
// so it flushes the heap every time, not only on the first.
void world_gpu_kick(void) {
	if (!s_gpuKicks)
		return;
	if (s_kicks)
		heapFlush();
	gpuKick();
}

void world_gpu_kick_clean(void) {
	if (!s_gpuKicks || s_kicks >= KICK_MAX - 2)
		return;
	gpuKick();   // the frame's first still flushes
}

void world_gpu_kick_early(void) {
	if (!s_gpuKicks || s_kicks >= KICK_MAX - 2)
		return;
	if (s_kicks)
		heapFlush();
	gpuKick();
}

int world_gpu_frame_end(u64* outFirstTick) {
	const int n = s_kicks;
	if (n) {
		heapFlush();
		gxCmdQueueWait(&__C3D_Context, -1);
		gxCmdQueueStop(&__C3D_Context);
		if (outFirstTick)
			*outFirstTick = s_kickFirst;
	}
	s_kicks = 0;
	return n;
}

void world_draw(const World* w, const C3D_Mtx* vp, const float pos[3],
		float ambient, float gain, float invGamma) {
	if (!w)
		return;
	// The sky and the clears are already down: the GPU can start on those.
	// A kick before this one (the env map's, #349, or the left eye's walk)
	// has run lists while the CPU went on writing the heap -- the reflection's
	// and the cars' vertices, the shadows -- so this one flushes again.
	if (s_gpuKicks)
		world_gpu_kick();
	Frustum fr;
	frustumFromVp(vp, &fr);
	// The same four planes the section test below uses, handed down to the
	// per-batch and per-cell tests inside each section (TASKS.md #68). They are
	// world-space, which is only true for geometry that is already in world
	// space -- so they are cleared again at the end of the loop, before the
	// shadow, the car or anything else drawn through a model matrix of its own
	// gets to model3d.c with boxes in model space.
	model3d_set_cull_planes(s_dbgNoCull ? NULL : fr.p);
	// DbgNearLod draws every instance at its A model, as before #247.
	if (s_lodViewSet && !s_dbgNearLod)
		model3d_set_lod_view(s_lodViewEye, s_lodViewFwd, s_lodViewH);
	else
		model3d_set_lod_view(NULL, NULL, 0.0f);
	// The frame the sections below work their instance states out in, which
	// the props' flares read after (world_instance_lod).
	s_drawLodFrame = model3d_lod_frame();
	s_instLooks = WORLD_INST_LOOKS;
	model3d_reset_draw_stats();
	int drawn = 0;
	// Two visits over the draw list, not one over each section (TASKS.md
	// #245). model3d.h's Model3DPhase says why: a section is one model, so
	// per-model opaque-then-blended still lets section A's neon race barriers
	// blend before section B's buildings are in the depth buffer, and the
	// barriers then glow through them. Every section's opaque geometry goes
	// down first, then every section's blended geometry, which is the same
	// split the batch loop already did, widened to the whole world.
	//
	// The visibility tests run twice rather than being cached in a list: they
	// are a vis lookup, a box distance and four plane tests over at most
	// WORLD_MAX_RESIDENT sections plus the pinned far scenery, which is far
	// cheaper than the draw they gate and keeps the two visits provably
	// identical.
	// The sections' positions relative to the camera (model3d.h, #376).
	model3d_set_world_origin(s_lodViewSet ? s_lodViewEye : pos);
	model3d_beam_defer(true);   // #412
	for (int phase = 0; phase < 2; phase++) {
		const Model3DPhase ph = phase == 0 ? MODEL3D_PHASE_OPAQUE
			: MODEL3D_PHASE_BLENDED;
		// The beam queued over the opaque phase goes down before anything
		// blends over it (#412).
		if (ph == MODEL3D_PHASE_BLENDED)
			model3d_beam_flush(vp);
		for (int i = 0; i < w->count; i++) {
			const WorldSection* s = &w->secs[i];
			if (!s->model)
				continue;
			if (s->flags & SECTION_ANIM)
				continue;
			if (s_dbgNoFarScenery && (s->flags & SECTION_FAR))
				continue;
			// Not on the retail list for where the car is. Before the first
			// lookup has an answer, everything resident draws.
			if (visHidden(w, i))
				continue;
			const float d2 = distSqToDrawBox(s, pos);
			if (!(s->flags & SECTION_FAR) && d2 > DRAW_RADIUS * DRAW_RADIUS)
				continue;
			if (aabbOutside(&fr, s->bbmin, s->bbmax))
				continue;
			// Only read for a pack with no instance table (see the note at
			// the top): its pinned far-scenery sections take their B models,
			// its street tiles their A. A section with a table picks per
			// instance and ignores this.
			const bool farLod = (s->flags & SECTION_FAR) != 0;
			// A wholly opaque section has nothing for the second visit, which
			// is most of them -- skipping saves the shader bind and the
			// uniform writes that a draw of nothing would still cost.
			if (ph == MODEL3D_PHASE_BLENDED && !model3d_has_blended(s->model))
				continue;
			model3d_draw_scene_lod_phase(s->model, vp, ambient, gain, invGamma,
				farLod, ph);
			if (s_gpuKicks && s_kicks < KICK_MAX &&
			    svcGetSystemTick() - s_kickLast >= KICK_TICKS)
				gpuKick();
			if (ph != MODEL3D_PHASE_OPAQUE)
				continue;
			drawn++;
		}
	}
	model3d_beam_flush(vp);   // no-op when the blended phase flushed it
	model3d_set_world_origin(NULL);
	// The props a car knocked loose (#318), each through a matrix of its own,
	// so with the world-space planes off. The LOD view stays: each picks its
	// level where it is now.
	model3d_set_cull_planes(NULL);
	if (s_propDraw)
		s_propDraw(w, vp, ambient, gain, invGamma, s_propDrawUd);
	model3d_draw_stats(&s_drawnCalls, &s_drawnTris);
	model3d_lod_stats(&s_drawnLodA, &s_drawnLodB);
	model3d_set_lod_view(NULL, NULL, 0.0f);
	s_drawnSections = drawn;
}

// One face of the car's env map (TASKS.md #166). The GameCube's env pass
// (elRenderEnvMapFaces) renders each face through its own eView, and
// DrawAScenery's 0x1000 mask then draws an instance only when it has flag
// 0x40 and not 0x10, at its Z model (B with 0x400) and at P >= 32 with the
// face camera's H, 256 at 90 degrees: model3d.c's env view. The sections are
// the player view's, the car's vis list and DRAW_RADIUS around the eye, cut
// to the face's frustum; nothing but scenery with an env table goes in.
// Returns the triangles submitted.
u32 world_draw_env(const World* w, const C3D_Mtx* vp, const float eye[3],
		const float fwd[3], float ambient, float gain, float invGamma) {
	if (!w)
		return 0;
	Frustum fr;
	frustumFromVp(vp, &fr);
	model3d_set_cull_planes(s_dbgNoCull ? NULL : fr.p);
	model3d_set_lod_view(eye, fwd, 256.0f);
	model3d_set_env_view(true);
	model3d_set_world_origin(eye);
	u32 calls0, tris0;
	model3d_draw_stats(&calls0, &tris0);
	for (int phase = 0; phase < 2; phase++) {
		const Model3DPhase ph = phase == 0 ? MODEL3D_PHASE_OPAQUE
			: MODEL3D_PHASE_BLENDED;
		for (int i = 0; i < w->count; i++) {
			const WorldSection* s = &w->secs[i];
			if (!s->model || !model3d_has_env(s->model) || (s->flags & SECTION_ANIM))
				continue;
			if (s_dbgNoFarScenery && (s->flags & SECTION_FAR))
				continue;
			if (visHidden(w, i))
				continue;
			if (!(s->flags & SECTION_FAR) &&
			    distSqToDrawBox(s, eye) > DRAW_RADIUS * DRAW_RADIUS)
				continue;
			if (aabbOutside(&fr, s->bbmin, s->bbmax))
				continue;
			if (ph == MODEL3D_PHASE_BLENDED && !model3d_has_blended(s->model))
				continue;
			if (!model3d_env_any(s->model))
				continue;
			model3d_draw_scene_lod_phase(s->model, vp, ambient, gain, invGamma,
				(s->flags & SECTION_FAR) != 0, ph);
		}
	}
	u32 calls1, tris1;
	model3d_draw_stats(&calls1, &tris1);
	model3d_set_world_origin(NULL);
	model3d_set_env_view(false);
	model3d_set_cull_planes(NULL);
	model3d_set_lod_view(NULL, NULL, 0.0f);
	return tris1 - tris0;
}

// The scenery of the road's reflection (#296, reflection.h). eDisplayFrame's
// views 4 and 5 are culled through SetupSceneryCullInfo(view, 0x800) with the
// player camera's frustum and H, and DrawAScenery under 0x800 draws the env
// set (0x40 and not 0x10, at the Z model) through each instance's matrix with
// its row 2 negated -- each prop upside down about its own origin's plane --
// less the 0x400 terrain StuffScenery's 8 leaves out. So the sections are
// world_draw()'s, the culling is the real boxes' in the real frustum, and
// model3d.c's reflection view does the rest. `vp` is the player's. Returns
// the triangles submitted; `outMirrors` the mirror changes they took.
u32 world_draw_reflection(const World* w, const C3D_Mtx* vp, const float pos[3],
		float ambient, float gain, float invGamma, u32* outMirrors) {
	if (outMirrors)
		*outMirrors = 0;
	if (!w || !s_lodViewSet)
		return 0;
	Frustum fr;
	frustumFromVp(vp, &fr);
	model3d_set_cull_planes(s_dbgNoCull ? NULL : fr.p);
	model3d_set_lod_view(s_lodViewEye, s_lodViewFwd, s_lodViewH);
	model3d_set_refl_view(vp);
	model3d_set_world_origin(s_lodViewEye);
	u32 calls0, tris0, rf0, mirrors0;
	model3d_draw_stats(&calls0, &tris0);
	model3d_reflection_stats(&rf0, &mirrors0);
	for (int phase = 0; phase < 2; phase++) {
		const Model3DPhase ph = phase == 0 ? MODEL3D_PHASE_OPAQUE
			: MODEL3D_PHASE_BLENDED;
		for (int i = 0; i < w->count; i++) {
			const WorldSection* s = &w->secs[i];
			if (!s->model || !model3d_has_env(s->model) || (s->flags & SECTION_ANIM))
				continue;
			if (s_dbgNoFarScenery && (s->flags & SECTION_FAR))
				continue;
			if (visHidden(w, i))
				continue;
			if (!(s->flags & SECTION_FAR) &&
			    distSqToDrawBox(s, pos) > DRAW_RADIUS * DRAW_RADIUS)
				continue;
			if (aabbOutside(&fr, s->bbmin, s->bbmax))
				continue;
			if (ph == MODEL3D_PHASE_BLENDED && !model3d_has_blended(s->model))
				continue;
			if (!model3d_env_any(s->model))
				continue;
			model3d_draw_scene_lod_phase(s->model, vp, ambient, gain, invGamma,
				(s->flags & SECTION_FAR) != 0, ph);
		}
	}
	u32 calls1, tris1, rf1, mirrors1;
	model3d_draw_stats(&calls1, &tris1);
	model3d_reflection_stats(&rf1, &mirrors1);
	model3d_set_world_origin(NULL);
	model3d_set_refl_view(NULL);
	model3d_set_cull_planes(NULL);
	model3d_set_lod_view(NULL, NULL, 0.0f);
	if (outMirrors)
		*outMirrors = mirrors1 - mirrors0;
	return tris1 - tris0;
}

void world_set_split_load(bool on) {
	s_splitLoad = on;
}

void world_set_lod_bias(float bias) {
	s_bankLodBias = bias;
	// Asking for a bias is asking for the chain it applies to.
	s_bankMips = true;
}

void world_force_tex_base(int level) {
	s_forceTexBase = level;
}

void world_set_mip_chains(bool on) {
	s_bankMips = on;
}

void world_set_ground_mips(bool on) {
	s_groundMips = on;
}

void world_set_tex_mode(World* w, WorldTexMode mode) {
	s_texMode = mode;
	if (!w || !s_bankMips)
		return;
	for (int i = 0; i < w->bankCount; i++) {
		if (w->bank[i].valid)
			bankApplyTexMode(w, &w->bank[i]);
	}
}

void world_set_tex_filter(World* w, bool linear) {
	const GPU_TEXTURE_FILTER_PARAM f = linear ? GPU_LINEAR : GPU_NEAREST;
	if (f == s_texFilter)
		return;
	s_texFilter = f;
	if (!w)
		return;
	for (int i = 0; i < w->bankCount; i++) {
		if (w->bank[i].valid)
			C3D_TexSetFilter(&w->bank[i].tex, f, f);
	}
}

void world_set_draw_debug(bool noFarScenery, bool nearLod, bool noCull) {
	s_dbgNoFarScenery = noFarScenery;
	s_dbgNearLod = nearLod;
	s_dbgNoCull = noCull;
}

bool world_spawn(const World* w, float out[3], float* outYaw) {
	if (!w || !w->count)
		return false;
	// The densest street tile wins -- most vertices per square metre of
	// footprint. That is a downtown block with buildings and props on it
	// rather than an empty ramp or a stretch of freeway, so the car starts
	// somewhere that looks like the city instead of somewhere technically
	// valid. Far-scenery sections are skipped outright: their centre is out
	// over the rooftops, not on a road.
	int best = -1;
	float bestDensity = -1.0f;
	for (int i = 0; i < w->count; i++) {
		const WorldSection* s = &w->secs[i];
		if (!s->numVerts || (s->flags & SECTION_FAR))
			continue;
		float area = (s->coremax[0] - s->coremin[0]) *
		             (s->coremax[1] - s->coremin[1]);
		if (area <= 1.0f)
			continue;
		float density = (float)s->numVerts / area;
		if (density > bestDensity) {
			bestDensity = density;
			best = i;
		}
	}
	if (best < 0)
		return false;
	const WorldSection* s = &w->secs[best];
	out[0] = (s->coremin[0] + s->coremax[0]) * 0.5f;
	out[1] = (s->coremin[1] + s->coremax[1]) * 0.5f;
	// Z is the section's floor, not its middle: the box's top is roof height.
	// It is not necessarily the street either -- a section that carries an
	// underpass has its floor down in the tunnel -- but picking the surface
	// under a given point is ground clamping, which is TASKS.md #3.
	out[2] = s->bbmin[2];

	// Face the middle of the world. With the whole track converted this is no
	// longer load-bearing the way it was when only a corner of L4RA existed
	// and yaw 0 drove straight off the edge of the data, but it still beats
	// picking an axis: from a tile near the edge of Bayview it points inland.
	if (outYaw) {
		float cx = (w->bbmin[0] + w->bbmax[0]) * 0.5f - out[0];
		float cy = (w->bbmin[1] + w->bbmax[1]) * 0.5f - out[1];
		*outYaw = (cx == 0.0f && cy == 0.0f) ? 0.0f : atan2f(cy, cx);
		if (*outYaw < 0.0f)
			*outYaw += 2.0f * (float)M_PI;
	}
	return true;
}

// world_ground_z() for either of topology's coordinates: the car's, or the
// camera's for the ground under the eye (#238).
static bool groundZ(const World* w, int coord, const float pos[3], float headroom,
	float* outZ)
{
	if (!w)
		return false;
	// Every resident section whose box covers the point gets asked, not just
	// the smallest one: section boxes overlap along their seams, and at a
	// seam the road under the car belongs to whichever tile happens to own
	// that triangle. Taking the highest answer also picks the overpass deck
	// over the street beneath it once the car is up on the deck, because the
	// street is then below `zMax` too and the deck is the higher of the two.
	bool hit = false;
	float best = 0.0f;
	const float zMax = pos[2] + headroom;

	// With topology there is one answer and it does not depend on what is
	// paged in, so the section loop is gone: see world_ground_probe().
	if (w->topo) {
		const float xy[2] = { pos[0], pos[1] };
		if (topo_ground_probe(w->topo, coord, xy, 1, zMax, &best, &hit, NULL, NULL) &&
		    hit && outZ)
			*outZ = best;
		return hit;
	}

	for (int i = 0; i < w->count; i++) {
		const WorldSection* s = &w->secs[i];
		if (!s->model || (s->flags & SECTION_FAR))
			continue;
		if (pos[0] < s->bbmin[0] || pos[0] > s->bbmax[0] ||
		    pos[1] < s->bbmin[1] || pos[1] > s->bbmax[1])
			continue;
		float z;
		if (!model3d_ground_z(s->model, pos[0], pos[1], zMax, &z))
			continue;
		if (!hit || z > best) {
			hit = true;
			best = z;
		}
	}
	if (hit && outZ)
		*outZ = best;
	return hit;
}

bool world_ground_z(const World* w, const float pos[3], float headroom, float* outZ)
{
	return groundZ(w, TOPO_COORD_CAR, pos, headroom, outZ);
}

// Batched sibling of world_ground_z(). Same rule about which sections get
// asked, but the points are handed to each section together, so four wheel
// contacts cost one pass over its triangles instead of four.
//
// outZ/outHit are initialised here and then accumulated across sections, so a
// car straddling a seam gets the highest surface from either side per wheel.
// Returns how many of the points found ground.
int world_ground_probe(const World* w, const float* xy, int n, float zMax,
	float* outZ, bool* outHit, float* outN)
{
	return world_ground_probe_coord(w, TOPO_COORD_CAR, xy, n, zMax, outZ, outHit, outN, NULL);
}

int world_ground_probe_coord(const World* w, int coord, const float* xy, int n,
	float zMax, float* outZ, bool* outHit, float* outN, signed char* outTerrain)
{
	if (n > MODEL3D_MAX_PROBES)
		n = MODEL3D_MAX_PROBES;
	for (int p = 0; p < n; p++) {
		outHit[p] = false;
		outZ[p] = 0.0f;
		if (outTerrain)
			outTerrain[p] = 1;
		if (outN) {
			outN[p * 3] = outN[p * 3 + 1] = 0.0f;
			outN[p * 3 + 2] = 1.0f;
		}
	}
	if (!w || n <= 0)
		return 0;

	// The points' own XY box, so a section covering none of them is rejected
	// before anything touches its triangle list.
	float xLo = xy[0], xHi = xy[0], yLo = xy[1], yHi = xy[1];
	for (int p = 1; p < n; p++) {
		float px = xy[p * 2], py = xy[p * 2 + 1];
		if (px < xLo) xLo = px; else if (px > xHi) xHi = px;
		if (py < yLo) yLo = py; else if (py > yHi) yHi = py;
	}

	// #236. Retail does not stand the car on the mesh it draws; it stands it
	// on the TopologyTree, which marks which surfaces are road and which are
	// only there for the solver -- roof tops, verges behind fences, the
	// insides of buildings. The drawn mesh carries no such distinction, so
	// the two are never mixed: letting the old scan contribute anywhere would
	// put the car back on the geometry this replaced, and it is resident in
	// full, so there is no seam for it to fall through either.
	if (w->topo) {
		topo_ground_probe(w->topo, coord, xy, n, zMax, outZ, outHit, outN, outTerrain);
	} else {
		for (int i = 0; i < w->count; i++) {
			const WorldSection* s = &w->secs[i];
			if (!s->model || (s->flags & SECTION_FAR))
				continue;
			if (xHi < s->bbmin[0] || xLo > s->bbmax[0] ||
			    yHi < s->bbmin[1] || yLo > s->bbmax[1])
				continue;
			model3d_ground_z_multi(s->model, xy, n, zMax, outZ, outHit, outN);
		}
	}

	int hits = 0;
	for (int p = 0; p < n; p++)
		if (outHit[p])
			hits++;
	return hits;
}

// ------------------------------------------------------------- wall collision
//
// One gather, then a resolve (TASKS.md #137, #141). The gather is the
// expensive half -- a scan of every resident section the car is over -- so it
// runs once per frame over a radius wide enough to cover the whole of that
// frame's travel, and the resolve then walks the move in substeps against the
// handful of segments that came back. Doing it the other way round, gathering
// per substep, is the same scan four to sixteen times for the same answer.
//
// The car is resolved as its own oriented footprint rather than as a circle.
// A circle the size of a car cannot fit down a lane between two parked cars
// and cannot be parallel-parked against a kerb, and it hits a wall with the
// same nose-on normal no matter how obliquely it was travelling, so glancing
// blows were as expensive as head-on ones. The box is the real body box out of
// the car record, so a Hummer does not fit where a Miata does.
//
// Substepping is not optional: at speed the car covers more than its own width
// in a frame, and a single test at the destination would put it straight
// through a facade without ever being inside one.
#define COLLIDE_MAX_WALLS   192
#define COLLIDE_MAX_STEPS    16
#define COLLIDE_ITERATIONS    4
#define COLLIDE_MAX_GATHER  24.0f   // m; the cap on a frame's travel + radius

static Model3DWall s_walls[COLLIDE_MAX_WALLS];

// One separating-axis test between the car's oriented footprint and one wall
// segment (TASKS.md #141). In 2D the only candidate axes are the box's two and
// the segment's own normal, and the axis with the least overlap gives both the
// shortest way out and the direction the wall faces -- which is why this one
// routine can feed the resolver its push and the physics its contact normal.
//
// Returns false when the two are apart, which is the common case, and bails on
// the first separating axis it finds rather than scoring all three.
static bool boxSegPush(float cx, float cy, float fx, float fy,
	float halfL, float halfW, const Model3DWall* seg,
	float* outPx, float* outPy)
{
	const float rx = fy, ry = -fx;          // the footprint's right axis
	float ex = seg->x1 - seg->x0, ey = seg->y1 - seg->y0;
	float elen = sqrtf(ex * ex + ey * ey);

	float axu[3], axv[3];
	axu[0] = fx; axv[0] = fy;
	axu[1] = rx; axv[1] = ry;
	int naxes = 2;
	if (elen > 1e-5f) {
		// A degenerate sliver has no meaningful normal of its own; the box's
		// two axes still separate it correctly, so it is simply left out.
		axu[2] = -ey / elen; axv[2] = ex / elen;
		naxes = 3;
	}

	float bestDepth = 1e30f, bestX = 0.0f, bestY = 0.0f;
	for (int i = 0; i < naxes; i++) {
		float ux = axu[i], uy = axv[i];
		// The box projects to a symmetric interval whose half-width is the sum
		// of its extents projected on the axis; the segment projects to the
		// span of its two ends.
		float r = halfL * fabsf(fx * ux + fy * uy) +
		          halfW * fabsf(rx * ux + ry * uy);
		float cp = cx * ux + cy * uy;
		float pa = seg->x0 * ux + seg->y0 * uy;
		float pb = seg->x1 * ux + seg->y1 * uy;
		float lo = pa < pb ? pa : pb;
		float hi = pa < pb ? pb : pa;

		// How far the box would have to move along +u, and along -u, to clear
		// the segment on this axis. Either being non-positive means the axis
		// already separates them, and then nothing else needs testing.
		float dPos = hi - (cp - r);
		float dNeg = (cp + r) - lo;
		if (dPos <= 0.0f || dNeg <= 0.0f)
			return false;

		float depth = dPos, sign = 1.0f;
		if (dNeg < dPos) { depth = dNeg; sign = -1.0f; }
		if (depth < bestDepth) {
			bestDepth = depth;
			bestX = ux * sign;
			bestY = uy * sign;
		}
	}

	*outPx = bestX * bestDepth;
	*outPy = bestY * bestDepth;
	return true;
}

// Where on the footprint a segment it overlaps is touching it, as an offset from
// the footprint's centre (TASKS.md #150). Retail applies its wall impulse at the
// contact point (Contact::CollisionImpulse, Speed.elf 0x801b24f4), and the lever
// arm is what turns a car that clips a wall nose-first back along it; an impulse
// through the centre only ever slows it.
//
// The point is the average of the box corners that are through the wall's line
// within the segment's span and the segment ends that are inside the box. Flat
// against a wall that is two corners and so the middle of that side; a corner
// into a wall is the corner; the end of a wall poking into a door is the end.
// Called only for a push that is being kept, so the cost is per hit, not per
// segment tested.
static void boxSegContact(float cx, float cy, float fx, float fy,
	float halfL, float halfW, const Model3DWall* seg, float px, float py,
	float* outAx, float* outAy)
{
	const float rx = fy, ry = -fx;
	float sx = 0.0f, sy = 0.0f;
	int k = 0;

	float ex = seg->x1 - seg->x0, ey = seg->y1 - seg->y0;
	float elen = sqrtf(ex * ex + ey * ey);
	if (elen > 1e-5f) {
		float tx = ex / elen, ty = ey / elen;
		// The wall's normal, turned to face the centre of the box.
		float wx = -ty, wy = tx;
		float line = seg->x0 * wx + seg->y0 * wy;
		if (cx * wx + cy * wy < line) { wx = -wx; wy = -wy; line = -line; }
		float t0 = seg->x0 * tx + seg->y0 * ty;
		float t1 = seg->x1 * tx + seg->y1 * ty;
		float tlo = t0 < t1 ? t0 : t1, thi = t0 < t1 ? t1 : t0;
		for (int c = 0; c < 4; c++) {
			float sl = (c & 1) ? halfL : -halfL;
			float sw = (c & 2) ? halfW : -halfW;
			float qx = cx + fx * sl + rx * sw, qy = cy + fy * sl + ry * sw;
			float qt = qx * tx + qy * ty;
			if (qx * wx + qy * wy < line && qt >= tlo && qt <= thi) {
				sx += qx; sy += qy; k++;
			}
		}
	}
	for (int e = 0; e < 2; e++) {
		float qx = e ? seg->x1 : seg->x0, qy = e ? seg->y1 : seg->y0;
		float lx = qx - cx, ly = qy - cy;
		if (fabsf(lx * fx + ly * fy) < halfL && fabsf(lx * rx + ly * ry) < halfW) {
			sx += qx; sy += qy; k++;
		}
	}

	if (k) {
		*outAx = sx / (float)k - cx;
		*outAy = sy / (float)k - cy;
		return;
	}
	// Nothing qualified, which only a grazing touch at an end can manage: the
	// corner furthest into the push is as good an answer as any.
	float sl = (fx * px + fy * py) > 0.0f ? -halfL : halfL;
	float sw = (rx * px + ry * py) > 0.0f ? -halfW : halfW;
	*outAx = fx * sl + rx * sw;
	*outAy = fy * sl + ry * sw;
}

// Pushes the footprint out of every segment it overlaps, repeatedly, because
// clearing one wall of a corner usually buries the car in the other. The push
// is along the shortest way out, so the part of the motion running parallel to
// the wall survives it -- that is the slide.
//
// Reports the direction of the DEEPEST push as the contact normal: that is the
// wall the car actually drove into, as opposed to a kerb it was already
// brushing, and it is what carsim uses to decide how much speed the hit costs.
// The same push's contact point goes with it, as an arm from the centre.
//
// A wall stops the box only where its face stands in the body band [zRef,
// zHi] at the contact (TASKS.md #280): its band at each end, along the wall
// to where the push meets it.
static bool resolveBox(const Model3DWall* walls, int n, float fx, float fy,
	float halfL, float halfW, float* x, float* y, float zRef, float zHi,
	float* deepest, float* outNx, float* outNy, float* outArm,
	WorldCollideInfo* info)
{
	bool pushed = false;
	for (int iter = 0; iter < COLLIDE_ITERATIONS; iter++) {
		bool any = false;
		for (int i = 0; i < n; i++) {
			float px, py;
			if (!boxSegPush(*x, *y, fx, fy, halfL, halfW, &walls[i], &px, &py))
				continue;
			const Model3DWall* s = &walls[i];
			if (s->z0min != s->z1min || s->z0max != s->z1max) {
				float ax, ay;
				boxSegContact(*x, *y, fx, fy, halfL, halfW, s, px, py, &ax, &ay);
				const float ex = s->x1 - s->x0, ey = s->y1 - s->y0;
				const float e2 = ex * ex + ey * ey;
				float u = e2 > 1e-8f ? ((*x + ax - s->x0) * ex + (*y + ay - s->y0) * ey) / e2 : 0.0f;
				u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
				if (s->z0max + (s->z1max - s->z0max) * u < zRef ||
				    s->z0min + (s->z1min - s->z0min) * u > zHi)
					continue;
			}
			float depth = sqrtf(px * px + py * py);
			if (depth > *deepest && depth > 1e-5f) {
				*deepest = depth;
				if (outNx) *outNx = px / depth;
				if (outNy) *outNy = py / depth;
				if (outArm)
					boxSegContact(*x, *y, fx, fy, halfL, halfW, &walls[i],
						px, py, &outArm[0], &outArm[1]);
			}
			*x += px;
			*y += py;
			if (info) {
				if (!info->blocking) {
					info->firstZLo = walls[i].zmin - zRef;
					info->firstZHi = walls[i].zmax - zRef;
				}
				info->blocking++;
			}
			any = true;
		}
		if (!any)
			break;
		pushed = true;
	}
	return pushed;
}

bool world_collide(const World* w, const float from[3], float to[3],
	float yaw, float halfLength, float halfWidth, float zLo, float zHi,
	float outNormal[2], float outArm[2], WorldCollideInfo* info)
{
	return world_collide_coord(w, TOPO_COORD_CAR, from, to, yaw, halfLength, halfWidth,
		zLo, zHi, outNormal, outArm, info);
}

bool world_collide_coord(const World* w, int coord, const float from[3], float to[3],
	float yaw, float halfLength, float halfWidth, float zLo, float zHi,
	float outNormal[2], float outArm[2], WorldCollideInfo* info)
{
	if (info) {
		info->gathered = info->blocking = 0;
		info->firstZLo = info->firstZHi = 0.0f;
	}
	if (outNormal)
		outNormal[0] = outNormal[1] = 0.0f;
	if (outArm)
		outArm[0] = outArm[1] = 0.0f;
	if (!w || !to)
		return false;

	float dx = to[0] - from[0], dy = to[1] - from[1];
	float travel = sqrtf(dx * dx + dy * dy);
	float reach = halfLength > halfWidth ? halfLength : halfWidth;
	if (travel <= 0.0f && reach <= 0.0f)
		return false;

	// The gather has to cover the footprint at its worst orientation, which is
	// the long half-extent, plus everywhere it goes this frame.
	float gather = reach + travel + 0.5f;
	if (gather > COLLIDE_MAX_GATHER)
		gather = COLLIDE_MAX_GATHER;

	// Same rule as the ground probe about which sections get asked: every
	// resident one whose box covers the query, widened here by the gather
	// radius so a wall just over a seam is not missed, and never the far
	// scenery -- a skyline plate is painted on, not something to crash into.
	// Not stopped when the buffer fills: model3d_walls_near() keeps the
	// nearest walls rather than the first ones, so every section still has to
	// be asked or a wall one tile over stays invisible to the car.
	//
	// Nor a section off the retail list for where the car is (#203). The x9x
	// district shells -- E94, E97, G97 and the like, low-detail stand-ins for
	// whole districts that are not flagged far -- are resident across most of
	// the city, and since #200 world_draw() leaves them out wherever retail
	// does. Colliding with them anyway was a wall the player cannot see: in F19
	// the car stopped dead on an open road against two E94 triangles. What is
	// not drawn is not hit. The ground queries keep asking them: a shell's
	// floor is metres under the real road (E97 sits at z 0-3 under F39's 17),
	// so it never wins there, and it still catches a car that finds a gap.
	//
	// All of which is about the drawn geometry, and none of it applies once
	// the pack has topology (#236). Retail authors 39,487 solid polygons for
	// the whole of L4RA and the car is stopped by those and nothing else, so
	// there is no steepness threshold to get wrong, no district shell to
	// collide with by accident, and no section list to walk: the data is
	// resident in full, and the walls come from the tree of the drivable
	// section the car is in (#238).
	int n = 0;
	if (w->topo) {
		n = topo_walls_near(w->topo, coord, from[0], from[1], gather,
			zLo, zHi, s_walls, 0, COLLIDE_MAX_WALLS);
	} else {
		for (int i = 0; i < w->count; i++) {
			const WorldSection* s = &w->secs[i];
			if (!s->model || (s->flags & SECTION_FAR) || visHidden(w, i))
				continue;
			if (from[0] < s->bbmin[0] - gather || from[0] > s->bbmax[0] + gather ||
			    from[1] < s->bbmin[1] - gather || from[1] > s->bbmax[1] + gather)
				continue;
			n = model3d_walls_near(s->model, from[0], from[1], gather, zLo, zHi,
				s_walls, n, COLLIDE_MAX_WALLS);
		}
	}
	if (info)
		info->gathered = n;
	if (!n)
		return false;

	// The substep bound is half the SHORT half-extent, because sideways is the
	// thin direction a wall can slip through between two tests.
	float thin = halfLength < halfWidth ? halfLength : halfWidth;
	int steps = 1;
	if (thin > 0.0f && travel > thin * 0.5f) {
		steps = (int)(travel / (thin * 0.5f)) + 1;
		if (steps > COLLIDE_MAX_STEPS)
			steps = COLLIDE_MAX_STEPS;
	}

	float fx = cosf(yaw), fy = sinf(yaw);
	float x = from[0], y = from[1];
	float sx = dx / (float)steps, sy = dy / (float)steps;
	float nx = 0.0f, ny = 0.0f, deepest = 0.0f;
	bool pushed = false;
	for (int i = 0; i < steps; i++) {
		x += sx;
		y += sy;
		if (resolveBox(s_walls, n, fx, fy, halfLength, halfWidth,
				&x, &y, zLo, zHi, &deepest, &nx, &ny, outArm, info))
			pushed = true;
	}
	to[0] = x;
	to[1] = y;
	if (outNormal) {
		outNormal[0] = nx;
		outNormal[1] = ny;
	}
	return pushed;
}

int world_walls_near_coord(const World* w, int coord, float x, float y, float radius,
	float zLo, float zHi, Model3DWall* out, int max)
{
	if (!w || !w->topo || max <= 0)
		return 0;
	return topo_walls_near(w->topo, coord, x, y, radius, zLo, zHi, out, 0, max);
}

int world_track_polys_coord(const World* w, int coord, const float box[4], u32* ids,
	float* boxes, int max)
{
	if (!w || !w->topo)
		return -1;
	return topo_track_polys(w->topo, coord, box, ids, boxes, max);
}

bool world_track_poly(const World* w, u32 id, TrackPoly* out)
{
	return w && w->topo && topo_track_poly(w->topo, id, out);
}

// ------------------------------------------------------------ camera wall gap
//
// TASKS.md #211. Retail keeps its outside cameras (CubicCameraMover::Update,
// Speed.elf 0x80072638, for the POVs OutsidePOV 0x80070ed8 accepts) out of the
// walls with CameraMover::MinGapTopology (0x8006b318), which works on the eye
// point after the follow has placed it:
//
//   1. TopologyCoordinate::ConstrainToTopology (0x801f20d0), type 1, two-sided:
//      the segment from the car's middle to the eye is tested against every
//      track polygon (TrackPolygon::TestSegmentToPoly 0x801f0b50), and if one
//      cuts it the eye comes in to MinDistToWall (0.7 m, 0x8006a8f4) short of
//      the nearest cut.
//   2. CameraMover::EnforceMinGapToWalls (0x8006a900): up to three passes in
//      which every polygon whose shaft the eye stands in -- inside the prism
//      the polygon sweeps along its normal, on its front side
//      (TrackPolygon::PointInShaft 0x801f0828) -- and nearer than 0.7 m pushes
//      it out along the normal to 0.7 m, the pushes of one pass averaged.
//
// The polygons here are the port's wall segments, so a wall the car cannot
// pass the camera cannot either, plus the ground under the eye for the road
// polygons retail also counts in step 2 (pushed straight up: the probe gives a
// height, not a normal). Not carried over: step 2 skips polygons whose terrain
// type sets TerrainTypeInfoTable+0xd, which is undecoded and which the port's
// walls do not carry; and a segment has no front face, so "front" is the side
// the car is on -- the side step 1 has just put the eye on.
#define CAM_MIN_GAP     0.7f   // CameraMover::MinDistToWall
#define CAM_GAP_PASSES  3

bool world_camera_gap(const World* w, const float target[3], float eye[3])
{
	if (!w || !target || !eye)
		return false;
	float dx = eye[0] - target[0], dy = eye[1] - target[1], dz = eye[2] - target[2];
	float len = sqrtf(dx * dx + dy * dy + dz * dz);

	// Retail's interest box (SetInterestBBox) is centred between the two
	// points with half-size (distance + 1) / 2. Widened by the gap here: a
	// segment's distance test is tighter than retail's box-overlap test.
	float mx = (eye[0] + target[0]) * 0.5f, my = (eye[1] + target[1]) * 0.5f;
	float radius = (len + 1.0f) * 0.5f + CAM_MIN_GAP;
	float zLo = (eye[2] < target[2] ? eye[2] : target[2]) - CAM_MIN_GAP;
	float zHi = (eye[2] > target[2] ? eye[2] : target[2]) + CAM_MIN_GAP;
	int n = 0;
	if (w->topo) {
		n = topo_walls_near(w->topo, TOPO_COORD_CAMERA, mx, my, radius,
			zLo, zHi, s_walls, 0, COLLIDE_MAX_WALLS);
	} else {
		for (int i = 0; i < w->count; i++) {
			const WorldSection* s = &w->secs[i];
			if (!s->model || (s->flags & SECTION_FAR) || visHidden(w, i))
				continue;
			if (mx < s->bbmin[0] - radius || mx > s->bbmax[0] + radius ||
			    my < s->bbmin[1] - radius || my > s->bbmax[1] + radius)
				continue;
			n = model3d_walls_near(s->model, mx, my, radius, zLo, zHi,
				s_walls, n, COLLIDE_MAX_WALLS);
		}
	}

	bool moved = false;
	// 1. The nearest wall cutting target -> eye.
	if (len > 1e-4f) {
		float hit = len;
		bool cut = false;
		for (int i = 0; i < n; i++) {
			const Model3DWall* s = &s_walls[i];
			float ex = s->x1 - s->x0, ey = s->y1 - s->y0;
			float den = dx * ey - dy * ex;
			if (fabsf(den) < 1e-6f)
				continue;
			float ax = s->x0 - target[0], ay = s->y0 - target[1];
			float t = (ax * ey - ay * ex) / den;   // along target -> eye
			float u = (ax * dy - ay * dx) / den;   // along the wall
			if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f)
				continue;
			float z = target[2] + dz * t;
			if (z < s->zmin || z > s->zmax)
				continue;
			if (t * len < hit) {
				hit = t * len;
				cut = true;
			}
		}
		if (cut) {
			// Retail's bNormalize to (hit - gap): a cut nearer than the gap
			// puts the eye on the far side of the car, as retail does.
			float k = (hit - CAM_MIN_GAP) / len;
			eye[0] = target[0] + dx * k;
			eye[1] = target[1] + dy * k;
			eye[2] = target[2] + dz * k;
			moved = true;
		}
	}

	// 2. The min-gap passes.
	for (int pass = 0; pass < CAM_GAP_PASSES; pass++) {
		float acc[3] = { 0.0f, 0.0f, 0.0f };
		int count = 0;
		for (int i = 0; i < n; i++) {
			const Model3DWall* s = &s_walls[i];
			if (eye[2] < s->zmin || eye[2] > s->zmax)
				continue;
			float ex = s->x1 - s->x0, ey = s->y1 - s->y0;
			float e2 = ex * ex + ey * ey;
			if (e2 < 1e-8f)
				continue;
			float px = eye[0] - s->x0, py = eye[1] - s->y0;
			float u = (px * ex + py * ey) / e2;
			if (u < 0.0f || u > 1.0f)
				continue;
			float side = ex * (target[1] - s->y0) - ey * (target[0] - s->x0);
			if (side == 0.0f)
				continue;
			float inv = (side > 0.0f ? 1.0f : -1.0f) / sqrtf(e2);
			float nx = -ey * inv, ny = ex * inv;   // toward the car's side
			float d = px * nx + py * ny;
			if (d >= 0.0f && d < CAM_MIN_GAP) {
				acc[0] += (CAM_MIN_GAP - d) * nx;
				acc[1] += (CAM_MIN_GAP - d) * ny;
				count++;
			}
		}
		float gz = 0.0f;
		if (groundZ(w, TOPO_COORD_CAMERA, eye, 0.0f, &gz)) {
			float d = eye[2] - gz;
			if (d >= 0.0f && d < CAM_MIN_GAP) {
				acc[2] += CAM_MIN_GAP - d;
				count++;
			}
		}
		if (!count)
			break;
		eye[0] += acc[0] / (float)count;
		eye[1] += acc[1] / (float)count;
		eye[2] += acc[2] / (float)count;
		moved = true;
	}
	return moved;
}

int world_section_count(const World* w) {
	return w ? w->count : 0;
}

int world_resident_count(const World* w) {
	return w ? w->resident : 0;
}

u32 world_resident_bytes(const World* w) {
	// Section blobs plus the bank pixels they are holding resident -- the two
	// halves of what the world is actually costing, and what the budget in
	// world_update() is checked against.
	return w ? w->residentBytes + w->bankBytes : 0;
}

u32 world_budget_bytes(const World* w) {
	return w ? w->budget : 0;
}

u32 world_bank_bytes(const World* w) {
	return w ? w->bankBytes : 0;
}

u32 world_tex_base_level(const World* w) {
	return w ? w->texBaseLevel : 0;
}

const char* world_section_at(const World* w, const float pos[3]) {
	if (!w)
		return NULL;
	// Smallest containing box, so a street tile is named ahead of the skyline
	// section whose box swallows it.
	const char* name = NULL;
	float bestArea = 1e30f;
	for (int i = 0; i < w->count; i++) {
		const WorldSection* s = &w->secs[i];
		if (!s->model || distSqToBox(s, pos) > 0.0f)
			continue;
		float area = (s->bbmax[0] - s->bbmin[0]) * (s->bbmax[1] - s->bbmin[1]);
		if (area < bestArea) {
			bestArea = area;
			name = s->name;
		}
	}
	return name;
}
