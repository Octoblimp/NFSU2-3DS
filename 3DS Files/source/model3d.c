#include "model3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <3ds.h>

#include "dbglog.h"
#include "gpuwatch.h"
#include "lightmat.h"
#include "shaders.h"

// Textures model3d_load_mem could not allocate (C3D_TexInitWithParams on a
// linear heap with no run that big), and their size, since boot (#413).
static u32 s_texAllocFails = 0;
static u32 s_texAllocFailKB = 0;

// TASKS.md #436: textures shared between models by their pixels. An entry
// holds the texture every borrower copies (a C3D_Tex is a handle: the copies
// point at the same pixels) and how many borrow it; the last one frees it.
// The key is two hashes over the format, size and every byte of the chain,
// so equal keys are equal art. A full table just stops sharing.
#define SHARE_MAX 384
typedef struct {
	u32     key[2];
	u32     bytes;
	s32     refs;
	C3D_Tex tex;
} ShareTex;
static ShareTex  s_share[SHARE_MAX];
static bool      s_shareOn = false;
static LightLock s_shareLock;
static bool      s_shareLockInit = false;
static u32       s_shareHits = 0, s_shareHitKB = 0;

static void shareLock(void) {
	if (!s_shareLockInit) {
		LightLock_Init(&s_shareLock);
		s_shareLockInit = true;
	}
	LightLock_Lock(&s_shareLock);
}

void model3d_share_textures(bool on) {
	s_shareOn = on;
}

void model3d_share_stats(u32* hits, u32* hitKB, u32* liveTex, u32* liveKB) {
	u32 n = 0, kb = 0;
	shareLock();
	for (int i = 0; i < SHARE_MAX; i++)
		if (s_share[i].refs > 0) {
			n++;
			kb += s_share[i].bytes / 1024u;
		}
	if (hits) *hits = s_shareHits;
	if (hitKB) *hitKB = s_shareHitKB;
	LightLock_Unlock(&s_shareLock);
	if (liveTex) *liveTex = n;
	if (liveKB) *liveKB = kb;
}

// FNV-1a and a second, differently seeded multiply-rotate pass, a word at a
// time; `bytes` is a whole number of words for every chain the packers write
// (8 x 8 texels and up), and any tail is folded in byte-wise.
static void shareKey(const u8* src, u32 bytes, u32 dim, u32 fld, u32 key[2]) {
	u32 a = 2166136261u ^ dim, b = 0x9E3779B9u ^ (fld * 0x85EBCA6Bu);
	const u32 words = bytes >> 2;
	for (u32 i = 0; i < words; i++) {
		u32 v;
		memcpy(&v, src + i * 4, 4);
		a = (a ^ v) * 16777619u;
		b = ((b ^ v) * 0xC2B2AE35u);
		b = (b << 13) | (b >> 19);
	}
	for (u32 i = words * 4; i < bytes; i++) {
		a = (a ^ src[i]) * 16777619u;
		b = ((b ^ src[i]) * 0xC2B2AE35u);
	}
	key[0] = a ^ bytes;
	key[1] = b ^ (bytes * 0x27D4EB2Fu);
}

// The live entry with this key, a reference taken; -1 when none.
static int shareTake(const u32 key[2], u32 bytes) {
	int found = -1;
	shareLock();
	for (int i = 0; i < SHARE_MAX && found < 0; i++)
		if (s_share[i].refs > 0 && s_share[i].bytes == bytes &&
				s_share[i].key[0] == key[0] && s_share[i].key[1] == key[1]) {
			s_share[i].refs++;
			s_shareHits++;
			s_shareHitKB += bytes / 1024u;
			found = i;
		}
	LightLock_Unlock(&s_shareLock);
	return found;
}

// A texture just built put in the share with one reference (its builder's);
// -1 when the table is full and the builder keeps it as its own.
static int shareAdd(const u32 key[2], u32 bytes, const C3D_Tex* t) {
	int slot = -1;
	shareLock();
	for (int i = 0; i < SHARE_MAX && slot < 0; i++)
		if (s_share[i].refs <= 0) {
			s_share[i].key[0] = key[0];
			s_share[i].key[1] = key[1];
			s_share[i].bytes = bytes;
			s_share[i].refs = 1;
			s_share[i].tex = *t;
			slot = i;
		}
	LightLock_Unlock(&s_shareLock);
	return slot;
}

static void shareRelease(int i) {
	if (i < 0 || i >= SHARE_MAX)
		return;
	bool last = false;
	C3D_Tex t;
	shareLock();
	if (s_share[i].refs > 0 && --s_share[i].refs == 0) {
		last = true;
		t = s_share[i].tex;
	}
	LightLock_Unlock(&s_shareLock);
	if (last)
		C3D_TexDelete(&t);
}

#define NFSC_MAGIC   0x4353464e   // 'NFSC' little-endian
// Versions 2 and 3 are both read. 3 added maxLevel in bits 8-11 of a
// TexEntry's fmt field, making that texture's run in the blob its whole mip
// chain instead of level 0 alone. A v2 file goes down the v3 path unchanged --
// its fmt field only ever held 0-2, so the level bits read 0 and the chain is
// one level long. Checked rather than assumed: over the 18998 TexEntry fields
// in every shipped .nfsc (cars, world sections, skydome, shadow, rooms) the
// high byte of that field is zero in all of them. Accepting both is what lets
// a repack of one packer's output land without invalidating the others'.
#define NFSC_VERSION_MIN 2
#define NFSC_VERSION     3
#define NFSC_HDR     0x40
#define VERTEX_SIZE  32           // pos[3] + normal-or-baked-colour[3] + uv[2]
_Static_assert(VERTEX_SIZE == MODEL3D_VERTEX_BYTES, "model3d.h states the vertex size");

// tools/car_pack.py's fmt field: the format in bits 0-7, maxLevel (mip levels
// below level 0) in bits 8-11. They share one u16 because the header is
// exactly 0x40 and full, and because this field has only ever held 0-2.
// car_pack.tex_fmt_field()/tex_fmt_of()/tex_fmt_level() are the writers.
#define TEXFMT_MASK   0x00ff
#define TEXFMT_LEVEL(f) (((f) >> 8) & 0xf)
#define TEXFMT_RGB565 0
#define TEXFMT_RGBA4  1
// 32bpp. No .nfsc carries it now: the car body's pack-time paint split, which
// did, gave way to the composited skin below (TASKS.md #296).
#define TEXFMT_RGBA8 2
// The car's skin (carskin.h): an RGBA8 entry with a size and a mip count but
// no pixels, which model3d_set_skin() fills once carskin.c has composited
// it. Its alpha is the paint's GLOSS, not coverage.
#define TEXFMT_SKIN 3

// tools/car_pack.py's shading field.
#define SHADING_LIT   0
#define SHADING_BAKED 1

// The render state a texture carries in its TPK record, packed by
// tools/track_pack.py's texture_material() into the u16 at TexEntry +0x0a.
// tools/extract_global_textures.py documents where each field comes from in
// the retail data and what its values mean.
//
// Before this the port drew every batch with one blend state, which is why
// OBJ_TRACKBARRIERB_NEON_ -- a red neon chevron on solid black that the retail
// renderer adds rather than blends -- came out as an opaque black square, and
// the hanging string lights with it.
#define MAT_BLEND(m)   ((m) & 3)        // 0 opaque, 1 normal blend, 2 additive

// Batch flag: draw this geometry, never collide with it. Set by
// track_pack.py on street signage; see BATCH_NO_COLLIDE there for why bit 31
// is the one that is free to borrow from the retail sub-mesh flags field.
#define BATCH_NO_COLLIDE 0x80000000u
// Batch flag on a frontend room: the turntable the car stands on (DISC01),
// which tools/scene_pack.py tags and model3d_draw_scene_turned() turns with
// the car.
#define BATCH_TURNTABLE 0x40000000u
// Batch flag on a frontend room (tools/scene_pack.py BATCH_FE_VALPHA, #105):
// the batch's colour slot carries its vertex alpha, which retail multiplies
// into the texture's -- the disc shadow SHD_SHADOWTILE_A, fading out to its
// rim. Honoured only on the room draw (s_feX2): on a car the same bit is the
// paint slot below, on a section a scenery-set bit.
#define BATCH_FE_VALPHA 0x04000000u
// Batch flags bits 24-26: which of a car's paint materials the batch takes
// (carkit.h CARKIT_PAINT_*), 0 being the body. Only carkit.c writes it -- the
// retail flags kit_pack.py copies through never use those bits -- so every
// other model reads 0 and is painted exactly as before. Bit 27 is
// MODEL3D_BATCH_PART_COND (#372 step 7).
#define BATCH_PAINT_SLOT(f) (((f) >> 24) & 0x7u)
// Batch flags bits 16-27 on a world section: the scenery set the batch's
// instances share, 0 for always drawn (TASKS.md #237, scenery.h). Read only
// on a model given a set table (model3d_set_scenery), because on a car the
// same bits are the paint slot above.
#define BATCH_SCENERY_SET(f) (((f) >> 16) & 0xFFFu)
// Batch flag on a world section: retail's sub-mesh flag 0x40, carried through
// untouched (track_pack.py BATCH_NO_COLLIDE) -- the wet road, which
// eDataRender::Render draws with ps_Model(2) and the reflection views' capture
// (#296, model3d_set_reflection). 5090 of L4RA's 28,018 sub-meshes.
#define BATCH_REFLECTIVE 0x40u
#define MAT_USAGE(m)  (((m) >> 2) & 3) // 0 unused, 1 punchthrough, 2 real alpha
#define MAT_USAGE_NONE         0
#define MAT_USAGE_PUNCHTHROUGH 1
#define MAT_USAGE_REAL_ALPHA   2
#define MAT_DEPTH(m)   (((m) >> 4) & 1)
// A world texture's scrollType (TASKS.md #269): 0 none, 1 smooth, 2 stepped,
// 3 a fixed offset. See model3d_scroll_add().
#define MAT_SCROLL(m)  (((m) >> 8) & 3)
#define MAT_BLEND_OPAQUE   0
#define MAT_BLEND_NORMAL   1
#define MAT_BLEND_ADDITIVE 2

// Fallback body paint, for a car with no profile behind it, and the colour a
// car batch with no texture is shaded with.
//
// A car that *does* have a profile behind it gets its real colour instead: the
// save's BASE_PAINT slot names a part, the part carries RED/GREEN/BLUE in
// GLOBALB's car-parts database, and paint_pack.py bakes that lookup into
// PAINT.nfsp. The body itself is the composited skin (carskin.c), which holds
// that colour already; this is left for the batches whose slot resolved to
// nothing.
//
// It has to stay in step with carskin.c's FALLBACK_PAINT, the same promo blue.
// TEV constant colours are 0xAABBGGRR.
#define CAR_PAINT_COLOR 0xFF96461C

// The colour every car drawn through drawCarWith() is painted with, set by
// model3d_set_car_paint() from the profile and left at the fallback until
// something does.
static u32 s_carPaint = CAR_PAINT_COLOR;

// Beside it, a colour per component paint slot (BATCH_PAINT_SLOT) for an
// assembled career car, TEV order; 0 means "the body colour". All zero unless
// model3d_set_car_slot_paints() said otherwise, so a stock car never sees it.
static u32 s_carSlotPaint[MODEL3D_CAR_PAINT_SLOTS];
static bool s_carSlotPaintAny = false;

// Framing for model3d_draw_car_solo(). The model is normalised so its longest
// axis is 1.0, then pushed CAR_VIEW_DIST down the view Z axis; CAR_VIEW_TILT is
// how far the camera looks down on it.
#define CAR_VIEW_FOV   40.0f
#define CAR_VIEW_DIST  1.5f
#define CAR_VIEW_TILT  0.26f      // ~15 degrees

#define CAR_AMBIENT    0.45f
#define CAR_DIFFUSE    0.55f

typedef struct {
	u32 firstIndex;
	u32 indexCount;
	s32 texIndex;
	u32 flags;
	// See model3d_build_cull_cells(). cellCount is 0 until it has run, and
	// stays 0 for every model nobody asks it of, which the draw loop reads as
	// "no cull, draw the whole batch" -- so a car, a room and the skydome are
	// untouched by any of this.
	u32 cellFirst;
	u32 cellCount;
	// A world section with an instance table (#247) has runs instead of
	// cells: runs[runFirst .. runFirst + runCount).
	u32 runFirst;
	u32 runCount;
	float lo[3];
	float hi[3];
} ModelBatch;

// One contiguous run of a batch's indices and the box over the vertices it
// touches: the leaf of the cull (see model3d_build_cull_cells).
typedef struct {
	float lo[3];
	float hi[3];
	u32   first;
	u32   count;
} CullCell;

// One instance of a world section's scenery (TASKS.md #247), exactly as
// tools/track_pack.py's pack_instance_block() writes it: what Speed.elf's
// DrawAScenery (0x80208fa4) picks the instance's level of detail from, every
// frame. See instancePick().
typedef struct {
	float lo[3];      // the scenery record's world box
	float hi[3];
	float pos[3];     // its translation, which the LOD distance is measured to
	float radius;     // SceneryInfo +0x3c + 6; < 0: no record, no LOD
	float density;    // max(A density, 6); 0 always A; < 0 never A
	u32   env;        // INST_ENV_*
} ModelInstance;
_Static_assert(sizeof(ModelInstance) == 48, "instance record is the file's");

// One unbroken stretch of a batch's indices that belongs to one instance, the
// leaf a batch is drawn in. A large one is cut further into boxed pieces
// (buildInstanced), and `box` names the piece's box in runBoxes; the rest are
// RUN_NO_BOX and answer to their instance's box alone, as retail's do.
typedef struct {
	u32 first;
	u16 tris;
	u16 inst;
	u32 box;
} ModelRun;
#define RUN_NO_BOX 0xFFFFFFFFu

typedef struct {
	float lo[3];
	float hi[3];
} RunBox;

// tools/track_pack.py's instance block.
#define INST_BLOCK_MAGIC  0x4953464e   // 'NFSI' little-endian
#define INST_BLOCK_HDR    16
#define INST_RECORD_SIZE  48
#define INST_RUN_SIZE     8

// What an instance draws this frame: its A model, its B model, and whether its
// box straddles the frustum (so a boxed piece of it still has a test to pass).
// The batch list is base / near / far (track_pack.py's tiers): a base batch is
// drawn for either level, a near one for A and a far one for B.
#define INST_DRAW_A    0x01u
#define INST_DRAW_B    0x02u
#define INST_STRADDLE  0x04u
// And a fourth tier after the far one, the env faces' own models (#166):
// drawn only in an env view, for the instances whose env model is there.
#define INST_DRAW_E    0x08u

// ModelInstance::env, track_pack.py's ENV_*: what the env faces draw of an
// instance -- nothing, its base or far run (INST_DRAW_B), or its run in the
// env tier (INST_DRAW_E) -- and whether the player view has a model for it at
// all.
#define INST_ENV_KIND  0x3u
#define INST_ENV_REUSE 0x1u
#define INST_ENV_OWN   0x2u
#define INST_ENV_ONLY  0x4u
// And how the reflection views draw it (#296, track_pack.py's mirror_row):
// not at all (the terrain), as it is (no matrix to mirror), or mirrored --
// about its translation's height when upright, else by row N of the NFSR
// table, N in the top 24 bits.
#define INST_ENV_NOREFL    0x08u
#define INST_ENV_FLAT      0x10u
#define INST_ENV_ROW_SHIFT 8
#define REFL_TABLE_MAGIC   0x5253464e   // 'NFSR' little-endian

// See model3d_build_phys_grid().
typedef struct PhysGrid {
	float x0, y0;     // the physics triangles' XY box min: cell (0,0)'s corner
	float cell;       // cell edge, m
	float inv;        // 1 / cell
	int   w, h;       // cells across X and across Y
	u32   refs;       // listings in tris[]
	u32*  start;      // w*h + 1: cell c lists tris[start[c] .. start[c + 1])
	u16*  tris;       // triangle numbers (first index / 3), ascending per cell
} PhysGrid;

struct Model3D {
	int      numBatches;
	int      numTextures;
	int      numIndices;
	int      numVerts;
	u32      shading;
	ModelBatch* batches;
	C3D_Tex* textures;    // .used is false for entries that failed to init
	bool*    texValid;
	u32*     texHashes;   // per slot, so a caller can resolve them elsewhere
	u16*     texMat;      // per slot: MAT_BLEND / MAT_USAGE / MAT_DEPTH
	// per slot: a texture animation's frame (#326), (anim << 8) | frame, or
	// 0xFFFF; NULL on a model none of whose textures animate.
	u16*     texAnim;
	// per slot: a TEXFMT_SKIN placeholder model3d_set_skin() uploads into.
	// Kept as its own array rather than read back off the record, because the
	// draw loop has the slot index and not the record.
	bool*    texSkin;
	// per slot: the share entry (#436) this slot's texture is borrowed from,
	// or -1 when the model owns it. NULL when nothing was shared.
	s16*     texShare;
	bool     anyBlended;  // true if any slot's MAT_BLEND is not opaque
	// True when this file carries real per-texture TPK render state, i.e. when
	// any slot's material word is non-zero. tools/track_pack.py fills it in,
	// and since TASKS.md #205 so do tools/car_pack.py and tools/kit_pack.py;
	// tools/scene_pack.py and the sky and shadow packers pass five-field
	// texture entries and get material 0 for every slot, so a zero everywhere
	// is indistinguishable from "opaque, no alpha, no depth write" and must not
	// be read as if the packer had asserted it. A car packed before #205 is
	// material 0 throughout and is drawn exactly as it was then.
	bool     hasMaterials;
	// Batch indices sorted into the two passes drawBatches() runs: the opaque
	// ones first, then the blended ones, with numOpaque the boundary. Built
	// once at load so neither pass walks batches belonging to the other -- a
	// section of several hundred batches with one neon sign in it was
	// otherwise scanned end to end twice per frame, and there are twenty-odd
	// resident sections. NULL when the model carries no material data, and
	// then both passes walk the whole list as before.
	u16*     passOrder;
	int      numOpaque;
	// The LOD split, read from the header's lodSplit field: the batch index
	// the near-detail run starts at, and the one the far-detail run starts at
	// (tools/track_pack.py bakes a world section's scenery at both levels and
	// lays the batch list out as base / near / far, each run contiguous). A
	// near draw is [0, lodFarFirst), a far one is [0, lodNearFirst) plus
	// [lodFarFirst, numBatches).
	//
	// A blob with no split -- every car, every room, the skydome, the shadow,
	// and any world blob built before track_pack.py wrote the field -- gets
	// both boundaries set to numBatches, which makes both of those ranges the
	// whole list. So nothing else has to know whether a model has LODs.
	int      lodNearFirst;
	int      lodFarFirst;
	// Where the env tier starts (#166), from the instance block's header:
	// the batches from here on are the env faces' models and nothing else
	// draws them. numBatches when there are none.
	int      envFirst;
	// Indices the ground and wall scans are allowed to look at: the far-detail
	// run is a second copy of geometry that is already in the near run, so
	// letting the car collide with it would mean colliding with every prop
	// twice, against a model authored to be looked at from 200 m rather than
	// driven into. The three runs are contiguous in the index pool in the same
	// order as in the batch list, so "everything but the far run" is simply a
	// shorter pool. numIndices when there is no split.
	int      physIndices;
	// Where in XY each of those triangles lies, so the ground and wall scans
	// can visit the few near the car instead of all of them. Built off the
	// frame by model3d_build_phys_grid(); NULL until then, and for any model
	// nobody asked it of, which the scans take as "walk the whole pool".
	struct PhysGrid* physGrid;
	// The cull leaves, batch by batch: batch b owns cells[cellFirst ..
	// cellFirst + cellCount). NULL until model3d_build_cull_cells() has run.
	CullCell* cells;
	u32       numCells;
	float     cullStep;   // the grid square it settled on, m, for reporting
	// The instance table (#247), for a world section packed with one: the
	// instances, this frame's INST_* state for each, and the runs every batch
	// is drawn as. NULL for everything else, which then draws as it always
	// has. `instBlock` is the block as read from the file; buildInstanced()
	// turns it into the rest and frees it.
	ModelInstance* insts;
	u32       numInsts;
	u8*       instState;
	u32       instFrame;  // the s_lodFrame instState was worked out for
	u8        instAny;    // every instState of this frame OR'd together
	// The same for the env view being drawn (model3d_set_env_view), kept
	// apart so a face cannot leave the player view a state it did not pick.
	// NULL when no instance is in a reflection.
	u8*       envState;   // numInsts this face's, then numInsts the eye's (#432)
	u32       envFrame;
	u8        envAny;
	u32       envDistGen; // s_envDistGen when the eye's half was worked out
	u8        envDistAny;
	// A bit per instance the smokeable props (smokeable.c, #318) have taken
	// out of the baked draw, in both views: a crash barrel a car knocked loose
	// is drawn where it is now, by model3d_draw_instance(). NULL until one is.
	u8*       hidden;
	// The box round the env instances' positions and the largest of their
	// radii, for skipping a model none of them can be picked in.
	float     envLo[3], envHi[3];
	float     envMaxR;
	// The NFSR table's mirror rows, 4 floats each (INST_ENV_ROW_SHIFT), and
	// whether the block had the table at all: a section packed before it
	// cannot tell its terrain from its props and is left out of a reflection.
	float*    reflRows;
	u32       numReflRows;
	bool      reflTable;
	ModelRun* runs;
	u32       numRuns;
	RunBox*   runBoxes;
	u32       numRunBoxes;
	u8*       instBlock;
	u32       instBlockBytes;
	// A car's own paints (TASKS.md #296): an AI car's body and component
	// colours, set by model3d_set_model_paints() and used in place of the
	// player's globals below when `ownPaint` is set.
	bool      ownPaint;
	// Its batches carry the car LODs they are drawn at (model3d_set_car_lods).
	bool      carLods;
	// Its batches include MODEL3D_BATCH_PART_LIT ones (model3d_set_car_bay).
	bool      carBay;
	// The rear rims' local +y, for TireFace's swap (model3d_set_car_rims).
	bool      rimAxes;
	float     rimAxis[2][3];
	// Where each corner's wheel parts turn about (model3d_set_car_pivots).
	bool      wheelPivots;
	float     wheelPivot[4][3];
	// A frontend room's turntable (BATCH_TURNTABLE): 0 not looked for yet,
	// 1 none, 2 found, centred at turnPivot in XY.
	u8        turnState;
	float     turnPivot[2];
	// One light material per batch (lightmat_index), or NULL: what the
	// in-world car is lit with (#166, model3d_set_light_mats).
	u8*       lightMat;
	// The car's glass (TASKS.md #324, model3d_set_glass): each window batch,
	// the window it is (0..5), the texture it was built with, and the three
	// cracked panes; glassLevel is what each window shows now.
	int       numGlass;
	u16*      glassBatch;
	u8*       glassWin;
	s32*      glassOrig;
	s32       glassTex[3];
	u8        glassLevel[MODEL3D_GLASS_WINDOWS];
	bool      ownSlotPaintAny;
	u32       ownPaintBody;
	u32       ownSlotPaint[MODEL3D_CAR_PAINT_SLOTS];
	// A world section carries a texture *table* but no pixels: they live in
	// the shared WORLD.NFSB bank, because the same road and brick art is
	// referenced by a dozen sections and paging each one in should not
	// re-upload art a neighbouring resident section already has. When set,
	// this is world.c's array of bank textures for this section's slots --
	// borrowed, not owned, and it wins over `textures`.
	C3D_Tex* const* extTextures;
	// What a batch whose texture never resolved is painted with (see
	// model3d_set_untextured_paint). White by default, which is a no-op.
	u32      untexPaint;
	// Which scenery sets are drawn, one bit per set (scenery.h), when this is
	// a world section; borrowed from world.c. NULL for every other model,
	// and then the set bits are not read at all.
	const u8* sceneryOn;
	void*    verts;       // linearAlloc: numVerts * VERTEX_SIZE
	u16*     indices;     // linearAlloc: numIndices * 2

	float    bbmin[3];
	float    center[3];
	float    scale;       // 1 / longest bbox axis, for the solo framing

	C3D_AttrInfo attr;
	C3D_BufInfo  buf;
};

// ----------------------------------------------------------------- shaders
//
// Two programs, one per `shading` mode, initialised on first use and never
// torn down (there is one car and one room, and each DVLB is a few hundred
// bytes).
typedef struct {
	DVLE_s*         dvle;     // the shared blob's (shaders.h, #352)
	shaderProgram_s program;
	bool            ready;
	int             uMvp, uNormalMtx, uLightDir, uShade;
	int             uUvOff;   // scene.v.pica only: the scroll offset
	int             uPosOff;  // and the world origin (model3d_set_world_origin)
} ModelShader;

static ModelShader s_carShader;
static ModelShader s_sceneShader;

static bool initShader(ModelShader* sh, ShaderId id, bool lit) {
	if (sh->ready)
		return true;
	sh->dvle = shaders_get(id);
	if (!sh->dvle)
		return false;
	shaderProgramInit(&sh->program);
	shaderProgramSetVsh(&sh->program, sh->dvle);
	sh->uMvp   = shaderInstanceGetUniformLocation(sh->program.vertexShader, "mvp");
	sh->uShade = shaderInstanceGetUniformLocation(sh->program.vertexShader, "shade");
	sh->uNormalMtx = lit
		? shaderInstanceGetUniformLocation(sh->program.vertexShader, "normalMtx") : 0;
	sh->uLightDir = lit
		? shaderInstanceGetUniformLocation(sh->program.vertexShader, "lightDir") : 0;
	sh->uUvOff = lit
		? 0 : shaderInstanceGetUniformLocation(sh->program.vertexShader, "uvoff");
	sh->uPosOff = lit
		? 0 : shaderInstanceGetUniformLocation(sh->program.vertexShader, "posoff");
	if (sh->uMvp < 0 || sh->uShade < 0 || sh->uNormalMtx < 0 || sh->uLightDir < 0 ||
			sh->uUvOff < 0 || sh->uPosOff < 0) {
		shaderProgramFree(&sh->program);
		sh->dvle = NULL;
		return false;
	}
	sh->ready = true;
	return true;
}

// ------------------------------------------------ texture UV scroll (#269)
//
// The textures that scroll, and this frame's offset for each. A handful at
// most -- 22 in the whole of Bayview, one of them the barriers' chevrons --
// so a batch that needs one finds it by a straight walk.
#define SCROLL_MAX 64
typedef struct {
	u32   hash;
	int   type;
	float step;          // scrollTimeStep / 256, s
	float speedS, speedT; // / 1024
	float offS, offT;    // / 1024
	float s, t;          // this frame's offset, in the PICA's UV space
} TexScroll;
static TexScroll s_scroll[SCROLL_MAX];
static int s_scrollCount;
// What the scene shader's uvoff holds now; drawBakedLod() zeroes it.
static float s_uvS, s_uvT;

void model3d_scroll_clear(void) {
	s_scrollCount = 0;
}

void model3d_scroll_add(u32 hash, int type, s16 timeStep, s16 speedS, s16 speedT,
		s16 offsetS, s16 offsetT) {
	if (type < 1 || type > 3 || s_scrollCount >= SCROLL_MAX)
		return;
	TexScroll* e = &s_scroll[s_scrollCount++];
	// The fixed-point scales are eDataRender::Render's constants, 0x80335DAC
	// (1/1024) and 0x80335DB4 (1/256).
	e->hash = hash;
	e->type = type;
	e->step = timeStep * (1.0f / 256.0f);
	e->speedS = speedS * (1.0f / 1024.0f);
	e->speedT = speedT * (1.0f / 1024.0f);
	e->offS = offsetS * (1.0f / 1024.0f);
	e->offT = offsetT * (1.0f / 1024.0f);
	e->s = e->t = 0.0f;
}

int model3d_scroll_count(void) {
	return s_scrollCount;
}

// TextureInfo::GetScroll (Speed.elf 0x800CFDAC): type 2 first truncates the
// time to whole steps, then both take the fractional part of time x speed --
// by truncation, so a negative speed gives an offset in (-1, 0].
static float getScroll(float time, float speed, int type, float step) {
	if (type == 2)
		time = (float)(int)(time / step);
	const float x = time * speed;
	return x - (float)(int)x;
}

// The texture animations (TASKS.md #326): the TextureAnimPacks the world's
// bank carries (tools/track_pack.py). UpdateTextureAnimations (Speed.elf
// 0x800cf2a4) gives entry i of an n-frame animation the pixels of entry
// (i + frame) mod n, frame = int(frames60 * fps / 60) mod n, on the world
// clock for timeBase 0 (every one the PC data ships). The PC only does it
// while g_AnimatedTextureEnable is on (FUN_005b9730); off, every texture
// keeps its own art. A section carries every frame of an animation it names
// as an extra slot (track_pack.py), so the swap is from one slot to another
// of the same model.
#define ANIM_MAX        16
#define ANIM_FRAMES_MAX 32
typedef struct {
	u32 frames[ANIM_FRAMES_MAX];
	u16 fps;
	u8  n;
	u8  k;               // this frame's offset into the frames
} TexAnim;
static TexAnim s_anim[ANIM_MAX];
static int s_animCount;
static bool s_animOn = true;
static u32 s_animDraws, s_animSwaps;   // since model3d_anim_stats() last ran

void model3d_set_world_timer(u32 ticks) {
	// WorldTimer is 1/4000 s (eDataRender::Render scales it by 0.00025).
	const float time = (float)(int)ticks * 0.00025f;
	for (int i = 0; i < s_scrollCount; i++) {
		TexScroll* e = &s_scroll[i];
		float s, t;
		if (e->type == 3) {
			s = -e->offS;
			t = -e->offT;
		} else {
			s = getScroll(time, e->speedS, e->type, e->step);
			t = getScroll(time, e->speedT, e->type, e->step);
		}
		// Retail translates (u, v) by (s, t). The packers store -v for the
		// PICA (track_pack.py), so the same move is -t here.
		e->s = s;
		e->t = -t;
	}
	// The texture animations (#326) run on the same clock: WorldTimeFrames,
	// which holds under the pause menu as this does.
	for (int i = 0; i < s_animCount; i++) {
		TexAnim* a = &s_anim[i];
		a->k = s_animOn ? (u8)((u32)(time * a->fps) % a->n) : 0;
	}
}

static const TexScroll* scrollFor(u32 hash) {
	for (int i = 0; i < s_scrollCount; i++)
		if (s_scroll[i].hash == hash)
			return &s_scroll[i];
	return NULL;
}


void model3d_anim_clear(void) {
	s_animCount = 0;
}

void model3d_anim_add(const u32* frames, int n, int fps) {
	if (n < 2 || n > ANIM_FRAMES_MAX || fps <= 0 || s_animCount >= ANIM_MAX)
		return;
	TexAnim* a = &s_anim[s_animCount++];
	memcpy(a->frames, frames, (size_t)n * sizeof(u32));
	a->n = (u8)n;
	a->fps = (u16)fps;
	a->k = 0;
}

int model3d_anim_count(void) {
	return s_animCount;
}

void model3d_anim_enable(bool on) {
	s_animOn = on;
}

void model3d_anim_stats(u32* draws, u32* swaps) {
	*draws = s_animDraws;
	*swaps = s_animSwaps;
	s_animDraws = s_animSwaps = 0;
}

static bool animFor(u32 hash, int* anim, int* frame) {
	for (int i = 0; i < s_animCount; i++)
		for (int f = 0; f < s_anim[i].n; f++)
			if (s_anim[i].frames[f] == hash) {
				*anim = i;
				*frame = f;
				return true;
			}
	return false;
}

// The slot whose art entry `ti` shows this frame, or NULL for its own.
static const C3D_Tex* animBind(const Model3D* m, s32 ti) {
	const int ai = m->texAnim[ti] >> 8, f = m->texAnim[ti] & 0xFF;
	if (ai >= s_animCount || f >= s_anim[ai].n || !m->texHashes)
		return NULL;
	const TexAnim* a = &s_anim[ai];
	const int j = (f + a->k) % a->n;
	if (j == f)
		return NULL;
	const u32 want = a->frames[j];
	for (int s = 0; s < m->numTextures; s++)
		if (m->texHashes[s] == want)
			return m->extTextures ? m->extTextures[s]
			                      : (m->texValid[s] ? &m->textures[s] : NULL);
	return NULL;
}

// -------------------------------------------------------------------- load

static u32 rd32(const u8* p, u32 off) {
	return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) | ((u32)p[off + 3] << 24);
}

static u16 rd16(const u8* p, u32 off) {
	return (u16)((u32)p[off] | ((u32)p[off + 1] << 8));
}

static float rdf32(const u8* p, u32 off) {
	union { u32 u; float f; } v;
	v.u = rd32(p, off);
	return v.f;
}

static bool isPow2(u32 v) {
	return v && !(v & (v - 1));
}

// Reading the bytes and turning them into a Model3D are separate because they
// cost wildly different amounts. Measured on the world's sections (#176): the
// parse, the allocations and the two memcpys below come to under 10 ms even on
// the largest section in L4RA, while the read that used to sit in front of
// them came to 400. world.c does the read on its loader thread and calls this
// on the main one, which is why this takes bytes and not a path.
//
// The buffer belongs to the caller and is not referenced after this returns:
// everything the GPU will touch is copied onto the linear heap here.
bool model3d_plan(const void* hdr, size_t hdrBytes, long fileSize,
                  Model3DPlan* p) {
	const u8* raw = (const u8*)hdr;
	if (!raw || !p || hdrBytes < NFSC_HDR || fileSize < NFSC_HDR)
		return false;
	if (rd32(raw, 0) != NFSC_MAGIC || rd32(raw, 4) < NFSC_VERSION_MIN ||
	    rd32(raw, 4) > NFSC_VERSION)
		return false;
	p->nTex     = rd32(raw, 0x08);
	p->nBatch   = rd32(raw, 0x0c);
	p->nVert    = rd32(raw, 0x10);
	p->nIdx     = rd32(raw, 0x14);
	p->texOff   = rd32(raw, 0x30);
	p->texSize  = rd32(raw, 0x34);
	p->shading  = rd32(raw, 0x38);
	p->lodSplit = rd32(raw, 0x3c);

	// Offsets of each section, in the order car_pack.py's pack_nfsc() writes
	// them.
	p->oTex   = NFSC_HDR;
	p->oBatch = p->oTex + p->nTex * 16;
	p->oVert  = p->oBatch + p->nBatch * 16;
	p->oIdx   = p->oVert + p->nVert * VERTEX_SIZE;
	const u32 idxEnd = p->oIdx + p->nIdx * 2;
	if (!p->nBatch || !p->nVert || !p->nIdx || p->nVert > 65536 ||
	    p->shading > SHADING_BAKED || idxEnd > (u32)fileSize ||
	    (u64)p->texOff + p->texSize > (u64)fileSize)
		return false;
	p->tableBytes = p->oVert - NFSC_HDR;
	p->vertBytes  = p->nVert * VERTEX_SIZE;
	p->idxBytes   = p->nIdx * 2;
	// A world section's instance table (#247) sits after the index pool's
	// padding and runs up to the texture blob; everything else has the blob
	// straight after the padding, and so none.
	p->instOff   = (idxEnd + 3u) & ~3u;
	p->instBytes = p->texOff > p->instOff ? p->texOff - p->instOff : 0;
	if (p->instBytes > (4u << 20))
		p->instBytes = 0;
	return true;
}

// g_TextureFiltering (#326, model3d_set_tex_filter): the filter a model's
// textures are built with.
static GPU_TEXTURE_FILTER_PARAM s_texFilter = GPU_LINEAR;

void model3d_set_tex_filter(bool linear)
{
	s_texFilter = linear ? GPU_LINEAR : GPU_NEAREST;
}

// The shared builder. `raw` holds at least the file's first p->oVert bytes --
// the header and the two tables -- and, when `whole` is set, the rest of the
// file as well, which is what lets the vertices, indices and any embedded
// pixels be taken from it directly. With `whole` clear the two GPU pools come
// back allocated but unwritten, for the caller to read into.
static Model3D* buildModel(const u8* raw, long fileSize, const Model3DPlan* p,
                           bool whole) {
	const u32 nTex     = p->nTex;
	const u32 nBatch   = p->nBatch;
	const u32 nVert    = p->nVert;
	const u32 nIdx     = p->nIdx;
	const u32 texOff   = p->texOff;
	const u32 texSize  = whole ? p->texSize : 0u;
	const u32 shading  = p->shading;
	const u32 lodSplit = p->lodSplit;
	const u32 oTex     = p->oTex;
	const u32 oBatch   = p->oBatch;
	const u32 oVert    = p->oVert;
	const u32 oIdx     = p->oIdx;
	(void)fileSize;

	Model3D* m = (Model3D*)calloc(1, sizeof(Model3D));
	if (!m)
		return NULL;
	m->numBatches  = (int)nBatch;
	m->numTextures = (int)nTex;
	m->numIndices  = (int)nIdx;
	m->numVerts    = (int)nVert;
	m->untexPaint  = 0xFFFFFFFF;
	m->shading     = shading;

	float bbmax[3];
	for (int i = 0; i < 3; i++) {
		m->bbmin[i] = rdf32(raw, 0x18 + i * 4);
		bbmax[i]    = rdf32(raw, 0x24 + i * 4);
	}
	float maxExtent = 0.0f;
	for (int i = 0; i < 3; i++) {
		m->center[i] = (m->bbmin[i] + bbmax[i]) * 0.5f;
		float e = bbmax[i] - m->bbmin[i];
		if (e > maxExtent) maxExtent = e;
	}
	m->scale = (maxExtent > 0.0f) ? (1.0f / maxExtent) : 1.0f;

	m->batches = (ModelBatch*)malloc(nBatch * sizeof(ModelBatch));
	// The GPU reads vertices and indices by physical address, so both pools
	// have to live on the linear heap rather than in the file buffer.
	m->verts   = linearAlloc(nVert * VERTEX_SIZE);
	m->indices = (u16*)linearAlloc(nIdx * 2);
	if (!m->batches || !m->verts || !m->indices) {
		model3d_free(m);
		return NULL;
	}
	for (u32 i = 0; i < nBatch; i++) {
		u32 o = oBatch + i * 16;
		m->batches[i].firstIndex = rd32(raw, o);
		m->batches[i].indexCount = rd32(raw, o + 4);
		m->batches[i].texIndex   = (s32)rd32(raw, o + 8);
		m->batches[i].flags      = rd32(raw, o + 12);
		m->batches[i].cellFirst  = 0;
		m->batches[i].cellCount  = 0;
		m->batches[i].runFirst   = 0;
		m->batches[i].runCount   = 0;
	}

	// The instance table rides along unparsed until the cull build, which
	// runs off the frame (model3d_build_cull_cells). A model that cannot get
	// the memory simply goes without, and draws as a model with no table.
	if (p->instBytes) {
		m->instBlock = (u8*)malloc(p->instBytes);
		if (m->instBlock) {
			m->instBlockBytes = p->instBytes;
			if (whole)
				memcpy(m->instBlock, raw + p->instOff, p->instBytes);
		}
	}

	// Both boundaries past the end unless the blob says otherwise, i.e. "one
	// level of detail, and it is drawn at every distance".
	m->lodNearFirst = m->lodFarFirst = (int)nBatch;
	m->envFirst     = (int)nBatch;
	m->physIndices  = (int)nIdx;
	if (lodSplit) {
		const int nf = (int)(lodSplit & 0xFFFF) - 1;
		const int ff = (int)(lodSplit >> 16) - 1;
		// A split that does not describe this batch list is ignored rather
		// than trusted: the cost of getting it wrong is drawing one level's
		// batches with the other level's index ranges.
		if (nf >= 0 && ff >= nf && ff <= (int)nBatch) {
			m->lodNearFirst = nf;
			m->lodFarFirst  = ff;
			if (ff < (int)nBatch && m->batches[ff].firstIndex <= nIdx)
				m->physIndices = (int)m->batches[ff].firstIndex;
		}
	}

	// Street signage is drawn but never collided with (#183): the car is meant
	// to knock a road sign over, and with every prop baked into the same merged
	// geometry the scans read, "never collided with" is the closest this port
	// can get to it. track_pack.py keeps those batches as a contiguous run at
	// the END of the near run and marks each one BATCH_NO_COLLIDE, so honouring
	// them costs one more integer cutoff rather than a per-batch test inside
	// three scans that are already the most expensive thing on the frame.
	//
	// Checked rather than trusted, because the failure is silent and one-way:
	// if the run is not contiguous, an unmarked batch after the cutoff would
	// quietly stop colliding, and a guardrail you can drive through is a worse
	// bug than a sign you cannot. So find the first marked batch, require
	// every batch from there to the far run to be marked too, and fall back to
	// the old cutoff -- everything collides -- if that does not hold.
	for (int i = 0; i < m->lodFarFirst; i++) {
		if (!(m->batches[i].flags & BATCH_NO_COLLIDE))
			continue;
		bool contiguous = true;
		for (int j = i + 1; j < m->lodFarFirst; j++) {
			if (!(m->batches[j].flags & BATCH_NO_COLLIDE)) {
				contiguous = false;
				break;
			}
		}
		if (contiguous && m->batches[i].firstIndex <= (u32)m->physIndices)
			m->physIndices = (int)m->batches[i].firstIndex;
		break;
	}
	// Only when the caller handed over the whole file. The pager has not read
	// the vertices or indices yet at this point -- its loader thread reads
	// them straight into these two pools once this returns.
	if (whole) {
		memcpy(m->verts,   raw + oVert, nVert * VERTEX_SIZE);
		memcpy(m->indices, raw + oIdx,  nIdx * 2);
	}

	if (nTex) {
		m->textures = (C3D_Tex*)calloc(nTex, sizeof(C3D_Tex));
		m->texValid = (bool*)calloc(nTex, sizeof(bool));
		m->texHashes = (u32*)calloc(nTex, sizeof(u32));
		m->texMat = (u16*)calloc(nTex, sizeof(u16));
		m->texSkin = (bool*)calloc(nTex, sizeof(bool));
		if (!m->textures || !m->texValid || !m->texHashes || !m->texMat ||
				!m->texSkin) {
			model3d_free(m);
			return NULL;
		}
		// #436: only a whole file has its pixels here to key on.
		if (s_shareOn && whole && texSize) {
			m->texShare = (s16*)malloc(nTex * sizeof(s16));
			for (u32 i = 0; m->texShare && i < nTex; i++)
				m->texShare[i] = -1;
		}
		for (u32 i = 0; i < nTex; i++) {
			m->texHashes[i] = rd32(raw, oTex + i * 16);
			m->texMat[i] = rd16(raw, oTex + i * 16 + 10);
			m->texSkin[i] = (rd16(raw, oTex + i * 16 + 8) & TEXFMT_MASK) == TEXFMT_SKIN;
			if (MAT_BLEND(m->texMat[i]) != MAT_BLEND_OPAQUE)
				m->anyBlended = true;
			if (m->texMat[i])
				m->hasMaterials = true;
			// A bank texture that scrolls (#269) is in the registry by hash,
			// which also covers a section packed before the scroll bits.
			if (!MAT_SCROLL(m->texMat[i])) {
				const TexScroll* e = scrollFor(m->texHashes[i]);
				if (e)
					m->texMat[i] |= (u16)(e->type << 8);
			}
			// A frame of a texture animation (#326).
			int ai, af;
			if (animFor(m->texHashes[i], &ai, &af)) {
				if (!m->texAnim && (m->texAnim = (u16*)malloc(nTex * sizeof(u16))) != NULL)
					memset(m->texAnim, 0xFF, nTex * sizeof(u16));
				if (m->texAnim)
					m->texAnim[i] = (u16)((ai << 8) | af);
			}
		}
		// texSize 0 means the pixels are in a shared bank; the caller resolves
		// the hashes and hands the result back via model3d_set_textures().
		for (u32 i = 0; texSize && i < nTex; i++) {
			u32 o = oTex + i * 16;
			u16 w    = rd16(raw, o + 4);
			u16 h    = rd16(raw, o + 6);
			u16 fld  = rd16(raw, o + 8);
			u16 fmt  = fld & TEXFMT_MASK;
			u32 dOff = rd32(raw, o + 12);
			// The PICA can only sample power-of-two textures of at least 8px.
			// The packers only ever halve already-PoT retail textures, so this
			// is a guard against a corrupt file, not a normal path.
			if (!isPow2(w) || !isPow2(h) || w < 8 || h < 8)
				continue;
			const bool rgba8 = fmt == TEXFMT_RGBA8;
			const bool skin = fmt == TEXFMT_SKIN;
			// The chain's levels are contiguous after level 0, each a quarter
			// of the one above. The bounds check has to cover all of them: a
			// file declaring more levels than it holds would otherwise have
			// the GPU read past the end of the mapping.
			u32 mlvl = TEXFMT_LEVEL(fld);
			if (mlvl > 12 || (w >> mlvl) < 8 || (h >> mlvl) < 8)
				continue;
			u32 lvl0 = (u32)w * h * (rgba8 || skin ? 4 : 2);
			u32 bytes = lvl0;
			for (u32 lv = 0, sz = lvl0; lv < mlvl; lv++) {
				sz >>= 2;
				bytes += sz;
			}
			if (!skin && (u64)texOff + dOff + bytes > (u64)fileSize)
				continue;
			GPU_TEXCOLOR gfmt = rgba8 || skin ? GPU_RGBA8
			                  : (fmt == TEXFMT_RGB565) ? GPU_RGB565 : GPU_RGBA4;
			// #436: the same pixels already up for a live model are borrowed.
			// The copy's own filter follows the setting as it is now.
			u32 shareK[2] = { 0, 0 };
			const bool tryShare = m->texShare && !skin;
			if (tryShare) {
				shareKey(raw + texOff + dOff, bytes, ((u32)w << 16) | h, fld, shareK);
				const int got = shareTake(shareK, bytes);
				if (got >= 0) {
					m->textures[i] = s_share[got].tex;
					C3D_TexSetFilter(&m->textures[i], s_texFilter, s_texFilter);
					m->texShare[i] = (s16)got;
					m->texValid[i] = true;
					continue;
				}
			}
			C3D_TexInitParams tp = { w, h, (u8)mlvl, gfmt, GPU_TEX_2D, false };
			if (!C3D_TexInitWithParams(&m->textures[i], NULL, tp)) {
				// The linear heap had no run this big: the batches on it draw
				// untextured (#413). Counted for the callers' logs.
				s_texAllocFails++;
				s_texAllocFailKB += bytes / 1024u;
				continue;
			}
			if (skin) {
				// Mid grey until model3d_set_skin(), rather than whatever the
				// linear heap held.
				u32 all = 0;
				void* px = C3D_TexGetImagePtr(&m->textures[i], m->textures[i].data, -1, &all);
				memset(px, 0x80, all);
			} else {
				// One load per level rather than a single C3D_TexUpload: the
				// levels are contiguous in the file but citro3d owns where each
				// one lands in the allocation, so let it place them.
				const u8* src = raw + texOff + dOff;
				u32 sz = lvl0;
				for (u32 lv = 0; lv <= mlvl; lv++) {
					C3D_TexLoadImage(&m->textures[i], src, GPU_TEXFACE_2D,
					                 (int)lv);
					src += sz;
					sz >>= 2;
				}
			}
			C3D_TexFlush(&m->textures[i]);
			C3D_TexSetFilter(&m->textures[i], s_texFilter, s_texFilter);
			// Minification interpolates between two levels instead of taking
			// one bilinear tap out of a full-size texture. The car is drawn
			// from 512px art (TASKS.md #188) and its panels turn away from the
			// camera, which is exactly where one tap crawls.
			if (mlvl)
				C3D_TexSetFilterMipmap(&m->textures[i], GPU_LINEAR);
			// The retail UVs tile (tyre treads, trim strips, brick courses),
			// and V was negated at pack time for the PICA's flipped V axis --
			// which is only equivalent to 1-v under repeat.
			C3D_TexSetWrap(&m->textures[i], GPU_REPEAT, GPU_REPEAT);
			m->texValid[i] = true;
			if (tryShare)
				m->texShare[i] = (s16)shareAdd(shareK, bytes, &m->textures[i]);
		}
	}

	// Sort the batches into the two passes now, so neither pass walks the
	// other's batches every frame (see Model3D::passOrder). Only worth doing
	// for a model that actually has blended material -- and only when the batch
	// count fits the u16 index, which every shipped section and car does by a
	// wide margin.
	if (m->anyBlended && m->texMat && nBatch <= 0xFFFF) {
		m->passOrder = (u16*)malloc(nBatch * sizeof(u16));
		if (m->passOrder) {
			int n = 0;
			for (u32 pass = 0; pass < 2; pass++) {
				for (u32 i = 0; i < nBatch; i++) {
					const s32 ti = m->batches[i].texIndex;
					const bool blended = (ti >= 0 && ti < m->numTextures) &&
						MAT_BLEND(m->texMat[ti]) != MAT_BLEND_OPAQUE;
					if (blended != (pass == 1))
						continue;
					m->passOrder[n++] = (u16)i;
				}
				if (pass == 0)
					m->numOpaque = n;
			}
		}
	}

	bool ok = (shading == SHADING_BAKED)
		? initShader(&s_sceneShader, SHADER_SCENE, false)
		: initShader(&s_carShader,   SHADER_CAR,   true);
	if (!ok) {
		model3d_free(m);
		return NULL;
	}

	AttrInfo_Init(&m->attr);
	AttrInfo_AddLoader(&m->attr, 0, GPU_FLOAT, 3);   // v0 position
	AttrInfo_AddLoader(&m->attr, 1, GPU_FLOAT, 3);   // v1 normal / baked colour
	AttrInfo_AddLoader(&m->attr, 2, GPU_FLOAT, 2);   // v2 texcoord
	BufInfo_Init(&m->buf);
	BufInfo_Add(&m->buf, m->verts, VERTEX_SIZE, 3, 0x210);

	return m;
}

Model3D* model3d_from_memory(const void* data, size_t size) {
	Model3DPlan p;
	if (!model3d_plan(data, size, (long)size, &p))
		return NULL;
	return buildModel((const u8*)data, (long)size, &p, true);
}

Model3D* model3d_from_plan(const Model3DPlan* p, const void* headerAndTables) {
	// A file carrying its own pixels needs them during the build, and they sit
	// past the vertices -- so that one wants the whole blob and the call above.
	// Only the debug --embed-textures packs are like that; the shipped world
	// shares one WORLD.NFSB bank and has texSize 0 throughout.
	if (!p || !headerAndTables || p->texSize)
		return NULL;
	return buildModel((const u8*)headerAndTables, (long)p->oVert, p, false);
}

void* model3d_vertex_dest(Model3D* m) { return m ? m->verts : NULL; }
int model3d_vertex_count(const Model3D* m) { return m ? m->numVerts : 0; }
void* model3d_index_dest(Model3D* m)  { return m ? m->indices : NULL; }

void model3d_relocate(Model3D* m, const void* from, void* to) {
	if (!m)
		return;
	if (m->verts == from) {
		m->verts = to;
		BufInfo_Init(&m->buf);
		BufInfo_Add(&m->buf, m->verts, VERTEX_SIZE, 3, 0x210);
	} else if (m->indices == from) {
		m->indices = (u16*)to;
	}
}
void model3d_set_streams(Model3D* m, const void* posNrm, const void* uv) {
	if (!m || !posNrm || !uv)
		return;
	BufInfo_Init(&m->buf);
	BufInfo_Add(&m->buf, posNrm, 24, 2, 0x10);
	BufInfo_Add(&m->buf, uv, 8, 1, 0x2);
}
void* model3d_inst_dest(Model3D* m)   { return m ? m->instBlock : NULL; }
const void* model3d_extra(const Model3D* m, u32* bytes) {
	*bytes = m && m->instBlock ? m->instBlockBytes : 0;
	return *bytes ? m->instBlock : NULL;
}

// Kept for every caller that is not the world pager -- cars, rooms, the
// skydome, the shadow. All of those load once behind a screen that is allowed
// to pause, so reading on the calling thread is fine for them.
Model3D* model3d_load(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fileSize < NFSC_HDR) {
		fclose(f);
		return NULL;
	}
	u8* raw = (u8*)malloc((size_t)fileSize);
	if (!raw) {
		fclose(f);
		return NULL;
	}
	const size_t got = fread(raw, 1, (size_t)fileSize, f);
	fclose(f);
	Model3D* m = (got == (size_t)fileSize)
		? model3d_from_memory(raw, (size_t)fileSize) : NULL;
	free(raw);
	return m;
}

void model3d_free(Model3D* m) {
	if (!m)
		return;
	if (m->textures) {
		for (int i = 0; i < m->numTextures; i++) {
			if (!m->texValid || !m->texValid[i])
				continue;
			if (m->texShare && m->texShare[i] >= 0)
				shareRelease(m->texShare[i]);
			else
				C3D_TexDelete(&m->textures[i]);
		}
		free(m->textures);
	}
	free(m->texShare);
	free(m->texValid);
	free(m->texHashes);
	free(m->texMat);
	free(m->texAnim);
	free(m->texSkin);
	free(m->lightMat);
	free(m->glassBatch);
	free(m->glassWin);
	free(m->glassOrig);
	free(m->passOrder);
	free(m->batches);
	free(m->cells);
	free(m->insts);
	free(m->instState);
	free(m->envState);
	free(m->hidden);
	free(m->reflRows);
	free(m->runs);
	free(m->runBoxes);
	free(m->instBlock);
	if (m->physGrid) {
		free(m->physGrid->start);
		free(m->physGrid->tris);
		free(m->physGrid);
	}
	if (m->verts)   linearFree(m->verts);
	if (m->indices) linearFree(m->indices);
	free(m);
}

bool model3d_is_scene(const Model3D* m) {
	return m && m->shading == SHADING_BAKED;
}

int model3d_texture_count(const Model3D* m) {
	return m ? m->numTextures : 0;
}

u32 model3d_texture_hash(const Model3D* m, int slot) {
	if (!m || !m->texHashes || slot < 0 || slot >= m->numTextures)
		return 0;
	return m->texHashes[slot];
}

C3D_Tex* model3d_own_texture(Model3D* m, int slot) {
	if (!m || !m->texValid || slot < 0 || slot >= m->numTextures || !m->texValid[slot])
		return NULL;
	return &m->textures[slot];
}

void model3d_set_textures(Model3D* m, C3D_Tex* const* textures) {
	if (m)
		m->extTextures = textures;
}

// -------------------------------------------------------------------- draw

// Shared GPU state for both models: they are drawn back to back into the same
// target through the same camera, so nothing here differs between them.
//
// GPU_GREATER against a depth buffer cleared to 0 is citro3d's convention and
// matches what C2D_TargetClear() leaves behind.
//
// Culling is off. The retail winding is D3D's; a car body is closed, and a
// room is a one-sided diorama whose backdrop shells are only ever seen from
// inside -- so culling would buy back a few thousand triangles at the risk of
// an inside-out model or a missing wall. Not worth it on the first pass.
// ------------------------------------------------------------ distance fog
//
// One LUT and one colour for the whole world pass. The LUT is rebuilt only
// when the terms change -- FogLut_Exp() is 128 expf/powf calls -- and binding
// it is what actually turns fog on, so the 2D passes can drop it in one call
// without losing the curve.
//
// zFlip is false, which is the one term here worth arguing about. The fog unit
// indexes the LUT with the depth buffer's own value, and FogLut_Exp() builds
// the table through FogLut_CalcZ(), which maps index 0 to `far` and index 1 to
// `near` -- that is, it already assumes the reversed depth this project's
// GPU_GREATER-against-a-zero-clear gives it, so flipping again would invert the
// gradient. Confirmed on hardware rather than argued: a diagnostic build with
// the density at 0.04 instead of 0.0036 left the near facade and the car
// untouched (62,60,47 -> 62,61,51) and pulled the far end of the same street to
// 38,48,62, within a couple of levels of the 36,48,68 fog colour.
//
// #328, on hardware alone: the fog unit's mode and its LUT change at once on
// the fragments of the draws still in flight, not after them. The 17:08
// build's burst on the 3DS (the 19:23 build's log): the wet overlay turning
// the fog off and on again around itself -- a mode write and a 128-word LUT
// upload each time -- left the near road pale blue in 3 frames of 4 and the
// whole tunnel washed in the fog colour now and then; the same overlay with
// the fog kept on at colour 0 was dark every time, and a framebuffer flush
// before the switch cleared it in 3 of 4. Azahar draws it right either way.
// So the LUT goes up only when the curve changes, never NULL'd, and a switch
// of the mode drains the GPU first (GPUREG_FRAMEBUFFER_FLUSH waits for the
// colour and depth writes of what is in flight).
static C3D_FogLut s_fogLut;
static u32  s_fogColor = 0;
static bool s_fogReady = false;
static bool s_fogOn    = false;
static bool s_fogMode  = false;   // the mode last given the GPU
static bool s_fogLutDirty = false;

// #400, on hardware alone: the first draw after a fog switch -- citro3d writes
// the new mode (GPUREG_TEXENV_UPDATE_BUFFER, 0xE0) lazily, right in front of
// that draw -- leaves nothing behind, neither colour nor depth. Every frame the
// sky is drawn unfogged and the street fogged, so the first world batch of the
// frame vanished: up close at section O30 that is a facade (its batch 28), and
// the graffiti wall behind it, in the same instance, showed through. Read off
// the console over GDB (2026-11-06): fog off, the facade is back; any draw put
// in front of it -- even one wholly behind the camera, no fragments at all --
// and it is back. Azahar draws it right either way. So the next draw after a
// switch is preceded by a sacrificial one: two zero-area triangles, which take
// the mode change and rasterise nothing. g_fogPrime: 0 off (the old way), 1 the
// prime, 2 the prime then a framebuffer flush.
u32 g_fogPrime = 1;
// #416: the same prime after a change of program. A tester's 3DS showed a
// patch of wet road (the mural wall under the elevated road, Azahar never)
// flicker between drawn and missing with the car standing still. The wet
// road's batches go through scenerf.v.pica and the rest through scene.v.pica,
// so the walk switches program batch by batch there -- the path #395 found
// the hardware loses batches on, which the beam left (hlOverlay) and the wet
// road did not. citro3d applies a program at the next draw
// (shaderProgramConfigure: the output map and attribute setup), so that draw
// is the first after the change, as #400's was after the fog mode. 0 turns
// it off, for an A/B over GDB.
u32 g_progPrime = 1;
static bool s_fogPrimePending = false;
static u16* s_fogPrimeIdx = NULL;
u32 g_fogPrimes = 0;   // primes drawn, for GDB

static float s_fogDensity, s_fogGradient, s_fogNear, s_fogFar;
static float (*s_fogMul)(float z, const void* ctx) = NULL;
static const void* s_fogMulCtx = NULL;

// FogLut_Exp's own loop (citro3d fog.c), each entry times the shape's
// multiplier at that entry's depth.
static void fogBuildLut(void) {
	if (!s_fogMul) {
		FogLut_Exp(&s_fogLut, s_fogDensity, s_fogGradient, s_fogNear, s_fogFar);
		return;
	}
	float data[256];
	for (int i = 0; i <= 128; i++) {
		const float x = FogLut_CalcZ((float)i / 128.0f, s_fogNear, s_fogFar);
		const float val = expf(-powf(s_fogDensity * x, s_fogGradient)) * s_fogMul(x, s_fogMulCtx);
		if (i < 128)
			data[i] = val;
		if (i > 0)
			data[i + 127] = val - data[i - 1];
	}
	FogLut_FromArray(&s_fogLut, data);
}

void model3d_fog_shape(float (*mul)(float z, const void* ctx), const void* ctx) {
	s_fogMul = mul;
	s_fogMulCtx = ctx;
	if (!s_fogReady)
		return;
	fogBuildLut();
	s_fogLutDirty = true;
}

void model3d_fog_set(u32 rgb, float density, float gradient, float nearZ, float farZ) {
	s_fogDensity = density;
	s_fogGradient = gradient;
	s_fogNear = nearZ;
	s_fogFar = farZ;
	fogBuildLut();
	// The fog unit's colour register is 0x00BBGGRR, the reverse of the
	// 0xRRGGBB callers think in.
	s_fogColor = ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu);
	s_fogReady = true;
	s_fogLutDirty = true;
	if (s_fogOn)
		model3d_fog_enable(true);
}

void model3d_fog_enable(bool on) {
	if (on && !s_fogReady)
		on = false;
	s_fogOn = on;
	const bool upload = on && s_fogLutDirty;
	if (on != s_fogMode || upload) {
		GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
		s_fogPrimePending = true;
	}
	s_fogMode = on;
	if (on) {
		C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
		C3D_FogColor(s_fogColor);
		if (upload) {
			C3D_FogLutBind(&s_fogLut);
			s_fogLutDirty = false;
		}
	} else {
		C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
	}
}

// Two surface-identification diagnostics, both off by default. Reading a
// texture's identity back off a screenshot by eye does not work -- mean colour
// is degenerate across 1591 textures and the vertex lighting moves it -- so
// these answer "what is actually on that surface" exactly.
//
//   g_dbgOnlyHash  draws only the batches carrying this texture hash. Whatever
//                  is left is the clear colour, so it also shows where a
//                  texture is NOT.
//   g_dbgHashTint  flat-shades every textured batch with the top three bytes
//                  of its hash. One screenshot then names every surface at
//                  once: match the pixel against the bank's hash list and the
//                  nearest entry is exact, not approximate.
//
// Kept because the ground-surface work (TASKS.md #191) is not finished and
// this is how it is measured.
u32  g_dbgOnlyHash = 0;
bool g_dbgHashTint = false;
// And one for what is drawn ON the surfaces: skips every model's blended pass,
// so a screenshot shows whether a colour belongs to the surface itself or to
// the glow and decal art laid over it.
bool g_dbgNoBlend = false;
// And the car light (#166): the old shader in its place, for comparing.
bool g_dbgCarPlainLight = false;

// The frontend room's faces cull (model3d_draw_scene_part(), MODEL3D_SCENE_ROOM):
// the platforms are wound with their fronts inward, and the shops' part
// camera puts its eye where only that works -- the hood vinyls' look sits
// 8 cm above PaintShop01's 5.42 m ceiling, which double-sided covers the
// shot with the ceiling's back.
static bool s_roomCull = false;
GPU_CULLMODE g_roomCullMode = GPU_CULL_BACK_CCW;

static void setCommonState(Model3DPass pass) {
	switch (pass) {
	case MODEL3D_PASS_SKY:
		// Drawn first, behind everything, at a radius that has nothing to do
		// with the depth range: testing or writing depth would either clip the
		// dome against the far plane or have it occlude the street.
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		break;
	case MODEL3D_PASS_DECAL:
		// Reads depth so the road in front of the car covers it, but writes
		// none: a shadow is not an occluder.
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
		break;
	default:
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		break;
	}
	C3D_CullFace(s_roomCull ? g_roomCullMode : GPU_CULL_NONE);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
}

// ------------------------------------------- sub-section frustum cull (#68)
//
// The four side planes of the frustum, in world space, exactly as world.c
// pulls them out of the view-projection matrix. NULL turns the cull off, which
// is what everything drawn with its own model matrix -- the car, the skydome,
// the shadow quad, a frontend room -- wants: their batch boxes are in model
// space and would be tested against the wrong thing.
static float s_cullPlanes[4][4];
static bool  s_cullOn = false;

// Which side of BATCH_TURNTABLE the batch loop keeps while
// model3d_draw_scene_turned() runs: TURN_ALL everywhere else.
enum { TURN_ALL, TURN_ROOM, TURN_DISC };
static int s_turnPass = TURN_ALL;

// A frontend room is being drawn (model3d_draw_scene_turned): its batches take
// SPEED2.EXE's FE effect, whose pixel shader (file 0x428738) is
// "mul r0, t0, v0; add r0.xyz, r0, r0" -- twice texture times the baked colour,
// saturated after the multiply, alpha not doubled. Its vertex shader (0x42878c)
// hands the baked colour through as it is.
static bool s_feX2 = false;
// The scene shader's `shade` as drawBakedLod() last set it, so a
// BATCH_FE_VALPHA batch can turn on shade.w for itself and put it back.
static float s_shadeNow[3];

// What the frame actually submitted, for the status overlay. The section count
// alone says nothing: a section culled down to two cells and one drawn whole
// both count as one.
static u32 s_drawCalls = 0;
static u32 s_drawTris  = 0;
// And how many scenery instances it drew at each level (#247).
static u32 s_lodDrawnA = 0;
static u32 s_lodDrawnB = 0;
// Bumped by every new view (model3d_set_cull_planes, model3d_set_lod_view),
// so a model works its instance states out once a frame however many passes
// draw it.
static u32 s_lodFrame = 1;

void model3d_set_cull_planes(const float planes[4][4])
{
	if (!planes) {
		s_cullOn = false;
		return;
	}
	memcpy(s_cullPlanes, planes, sizeof(s_cullPlanes));
	s_cullOn = true;
	// New planes are a new frame for the instance states too, whether or not
	// a LOD view was set with them.
	s_lodFrame++;
}

// The projected headlight (#296, model3d.h): this frame's projector, the
// program that draws it and what it drew.
static bool s_hlOn = false;
static Model3DHeadlight s_hl;
static ModelShader s_hlShader;
static int s_uHl = -1;
static u32 s_hlBatches, s_hlSwitches;
// #412: what hlOverlay() drew, in triangles, and whether its runs and cells
// are cut to the beam's own frustum. A tester's 3DS dropped frames in the
// bright, built-up spots (the body shop's entrance, a back alley), and the
// user's own 3DS log put up to 200 beam batches a frame there, each one
// whole: a street tile with one corner in the beam laid the beam over all of
// it. The beam's texel is zero outside its six planes (carlights.c), so a run
// or cell box wholly outside them adds nothing, and is no longer drawn.
// 0 over GDB draws the whole batches again, for an A/B.
u32 g_hlRunCull = 1;
static bool s_hlCull = false;
// #412 one pass: the plain draw of a batch whose beam pieces are queued for
// the one-pass flush skips exactly the runs and cells s_hlCull keeps, so each
// piece is drawn once -- plain outside the beam, through scenehl inside it.
static bool s_hlSkip = false;
static u32 s_hlTris;
static u32 s_hlFullBatches;   // #412: of s_hlBatches, drawn one-pass
static bool hlBoxIn(const float lo[3], const float hi[3]);
// #395: true draws the beam the pre-#395 way, each batch in the beam through
// scenehl.v.pica in place of its plain draw (model3d_set_beam_inline, the
// config's DbgNoBeam=3); false lays it over the plain draw (hlOverlay).
static bool s_hlInline = false;

void model3d_set_beam_inline(bool on)
{
	s_hlInline = on;
}
// The road's reflection's (#296): reflective batches drawn with the capture,
// and instance mirrors put up in a reflection view.
static u32 s_rfBatches, s_reflMirrors;

// The CPU split of the draws (#345, DbgCpuSplit=1): ticks in updateInstances()
// (the per-instance LOD and cull, pure arithmetic that could run on another
// core) and inside C3D_DrawElements() (citro3d flushing the dirty state and
// writing the draw into the command list, which has to stay on the render
// core). Everything else in a model draw is the batch walk: the batch and run
// tests and the state calls that only mark citro3d's state dirty. Off, the
// cost is one test of a bool per draw.
static bool s_cpuSplit = false;
static u64  s_cpuInst, s_cpuEmit;

void model3d_set_cpu_split(bool on)
{
	s_cpuSplit = on;
}

void model3d_cpu_split(u64* outInst, u64* outEmit)
{
	if (outInst) *outInst = s_cpuInst;
	if (outEmit) *outEmit = s_cpuEmit;
	s_cpuInst = s_cpuEmit = 0;
}

// Program changes: citro3d applies one at the next draw (shaderProgramConfigure,
// and the whole code blob when the new program's DVLB is another one), so a
// draw right after a change is timed apart from the rest.
static const shaderProgram_s* s_lastProg;
static bool s_progChanged;
static u32  s_cpuDraws, s_cpuSwitches;
static u64  s_cpuEmitSw;

// Since #352 every program here comes from one blob (shaders.h), so a change
// of program no longer changes the code; what is timed apart is a draw after
// a code change, which is what still costs (another module's blob, or one
// left over from before).
static inline void bindProg(shaderProgram_s* prog)
{
	if (prog != s_lastProg) {
		s_progChanged = !s_lastProg ||
			s_lastProg->vertexShader->dvle->dvlp != prog->vertexShader->dvle->dvlp;
		s_lastProg = prog;
		if (g_progPrime)
			s_fogPrimePending = true;   // #416, see g_progPrime
	}
	C3D_BindProgram(prog);
}

void model3d_cpu_split_counts(u32* outDraws, u32* outSwitches, u64* outSwitchTicks)
{
	if (outDraws) *outDraws = s_cpuDraws;
	if (outSwitches) *outSwitches = s_cpuSwitches;
	if (outSwitchTicks) *outSwitchTicks = s_cpuEmitSw;
	s_cpuDraws = s_cpuSwitches = 0;
	s_cpuEmitSw = 0;
}

// What the batch walk did (#352), for the CPU split's log: the sections'
// batches looked at, the ones a box test dropped, the ones drawn, the runs
// walked, the run boxes tested and the TEV rebuilds.
u32 g_walk[MODEL3D_WALK_COUNT];

void model3d_walk_stats(u32 out[MODEL3D_WALK_COUNT])
{
	memcpy(out, g_walk, sizeof(g_walk));
	memset(g_walk, 0, sizeof(g_walk));
}

// #400's prime (see g_fogPrime): drawn through the model's own program, buffers
// and state, so citro3d emits the pending fog mode in front of it, not in
// front of the real draw. Index 0 three times over is a point, not a triangle.
static void fogPrimeDraw(void)
{
	s_fogPrimePending = false;
	if (!g_fogPrime)
		return;
	if (!s_fogPrimeIdx) {
		s_fogPrimeIdx = (u16*)linearAlloc(16);
		if (!s_fogPrimeIdx)
			return;
		memset(s_fogPrimeIdx, 0, 16);
		GSPGPU_FlushDataCache(s_fogPrimeIdx, 16);
	}
	C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, s_fogPrimeIdx);
	if (g_fogPrime == 2)
		GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
	g_fogPrimes++;
}

// #486, on hardware alone: a tester's 3DS on the first-run display level
// (3: Light Trails and Over Bright on, no bleach) flashed the top screen a
// flat blue-grey for one frame, twice half a second apart -- the fog colour
// (36,48,68) added over the whole frame, which is what a fogged ONE / ONE
// full-screen quad at window depth paints. Azahar never showed it, even
// sampling every frame at the spot. The post passes (trails.c, smear.c,
// bleach.c) each bind their own program and draw through citro3d directly,
// so neither #400's fog prime nor #416's program prime ever covered them:
// the fog-off before the neon is lazy, and the first draw after a change of
// program is the one the hardware has been seen to lose. So each post pass,
// once its program and quad are bound, comes here: the fog mode written off
// again (re-emitted even when citro3d thinks it already is) and a zero-area
// draw through the pass's own buffers in front of its first real one, to
// take both changes. model3d's own next draw primes again (its program is no
// longer the one it last bound).
void model3d_post_pass_prime(void)
{
	model3d_fog_enable(false);
	s_lastProg = NULL;
	if (g_fogPrime)
		fogPrimeDraw();
	s_fogPrimePending = true;
}

static inline void drawElems(int count, const void* indices)
{
	if (s_fogPrimePending)
		fogPrimeDraw();
	if (!s_cpuSplit) {
		s_progChanged = false;
		C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, indices);
		return;
	}
	const bool sw = s_progChanged;
	s_progChanged = false;
	const u64 t0 = svcGetSystemTick();
	C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, indices);
	const u64 dt = svcGetSystemTick() - t0;
	s_cpuEmit += dt;
	s_cpuDraws++;
	if (sw) {
		s_cpuSwitches++;
		s_cpuEmitSw += dt;
	}
}

void model3d_reset_draw_stats(void)
{
	s_drawCalls = 0;
	s_drawTris  = 0;
	s_lodDrawnA = 0;
	s_lodDrawnB = 0;
	s_hlBatches = 0;
	s_hlSwitches = 0;
	s_hlTris = 0;
	s_hlFullBatches = 0;
	s_rfBatches = 0;
	s_reflMirrors = 0;
}

bool model3d_set_headlight(const Model3DHeadlight* hl)
{
	s_hlOn = false;
	if (!hl || !hl->beam || !hl->clip || !s_sceneShader.ready)
		return false;
	if (!s_hlShader.ready) {
		if (!initShader(&s_hlShader, SHADER_SCENEHL, false))
			return false;
		s_uHl = shaderInstanceGetUniformLocation(s_hlShader.program.vertexShader, "hl");
	}
	// scenehl.v.pica declares scene.v.pica's uniforms first so that a batch
	// can switch programs mid-section and keep them; a layout that moved
	// would light the world with the wrong matrix, so it is refused.
	if (s_uHl < 0 || s_hlShader.uMvp != s_sceneShader.uMvp ||
	    s_hlShader.uShade != s_sceneShader.uShade ||
	    s_hlShader.uUvOff != s_sceneShader.uUvOff ||
	    s_hlShader.uPosOff != s_sceneShader.uPosOff)
		return false;
	s_hl = *hl;
	s_hlOn = true;
	return true;
}

void model3d_headlight_stats(u32* outBatches, u32* outSwitches)
{
	if (outBatches)  *outBatches  = s_hlBatches;
	if (outSwitches) *outSwitches = s_hlSwitches;
}

u32 model3d_headlight_tris(void)
{
	return s_hlTris;
}

// The road's reflection (#296, model3d.h): this frame's capture and the
// program the reflective batches take it through.
static bool s_rfOn = false;
static const C3D_Tex* s_rfTex;
static ModelShader s_rfShader;

bool model3d_set_reflection(const C3D_Tex* capture)
{
	s_rfOn = false;
	if (!capture || !s_sceneShader.ready)
		return false;
	if (!s_rfShader.ready &&
	    !initShader(&s_rfShader, SHADER_SCENERF, false))
		return false;
	// Switched to mid-section like scenehl.v.pica, so its layout has to be
	// scene.v.pica's (see model3d_set_headlight).
	if (s_rfShader.uMvp != s_sceneShader.uMvp || s_rfShader.uShade != s_sceneShader.uShade ||
	    s_rfShader.uUvOff != s_sceneShader.uUvOff || s_rfShader.uPosOff != s_sceneShader.uPosOff)
		return false;
	s_rfTex = capture;
	s_rfOn = true;
	return true;
}

void model3d_reflection_stats(u32* outBatches, u32* outMirrors)
{
	if (outBatches) *outBatches = s_rfBatches;
	if (outMirrors) *outMirrors = s_reflMirrors;
}

// #328's batch dump (model3d.h): lines still to write, the draw order so far
// and the view-projection drawBakedLod() last bound.
static int s_dumpLeft;
static int s_dumpSeq;
static C3D_Mtx s_dumpVp;

void model3d_batch_dump(int lines)
{
	s_dumpLeft = lines > 0 ? lines : 0;
	s_dumpSeq = 0;
}

// One drawn batch. Its box's corners go through s_dumpVp; Mtx_PerspTilt puts
// the screen's height on clip x, so the row is (1 - x/w) * 120 and the column
// (1 - y/w) * 200, the 400x240 frame the probe shots hold. "behind" when a
// corner is at or behind the eye.
static void dumpBatch(const Model3D* m, int i, const ModelBatch* b, s32 ti, int prog,
		bool overlay, bool blendPass, int cullState)
{
	float r0 = 1e9f, r1 = -1e9f, c0 = 1e9f, c1 = -1e9f;
	bool behind = false;
	for (int k = 0; k < 8; k++) {
		const float p[3] = { (k & 1) ? b->hi[0] : b->lo[0], (k & 2) ? b->hi[1] : b->lo[1],
		                     (k & 4) ? b->hi[2] : b->lo[2] };
		float c[4];
		for (int j = 0; j < 4; j++) {
			const C3D_FVec* v = &s_dumpVp.r[j];
			c[j] = v->x * p[0] + v->y * p[1] + v->z * p[2] + v->w;
		}
		if (c[3] <= 0.01f) {
			behind = true;
			continue;
		}
		const float row = (1.0f - c[0] / c[3]) * 120.0f, col = (1.0f - c[1] / c[3]) * 200.0f;
		if (row < r0) r0 = row;
		if (row > r1) r1 = row;
		if (col < c0) c0 = col;
		if (col > c1) c1 = col;
	}
	static const char* const kProg[] = { "scene", "beam", "wet" };
	char line[200];
	snprintf(line, sizeof(line), "batch dump %d: model %08lx batch %d tex %08lx flags %04lx, %s%s, "
		"%s pass, cull %d; box rows %.0f-%.0f cols %.0f-%.0f%s", s_dumpSeq,
		(unsigned long)((uintptr_t)m & 0xFFFFFFFFu), i,
		(unsigned long)(ti >= 0 && m->texHashes ? m->texHashes[ti] : 0),
		(unsigned long)(b->flags & 0xFFFFu), kProg[prog], overlay ? " + wet overlay" : "",
		blendPass ? "blended" : "opaque", cullState, r0, r1, c0, c1, behind ? ", behind" : "");
	dbglog_line(NULL, line);
	s_dumpSeq++;
	s_dumpLeft--;
}

void model3d_draw_stats(u32* outCalls, u32* outTris)
{
	if (outCalls) *outCalls = s_drawCalls;
	if (outTris)  *outTris  = s_drawTris;
}

// eViewPlatInterface::GetVisibleStateSB (Speed.elf 0x801e9608), which is what
// eView::GetVisibleState and so IsAABBCurrentlyVisible come down to: the box as
// a centre and a half-extent, `r` its radius along the plane normal and `d` the
// centre's signed distance, so d + r < 0 is outside and d - r < 0 a straddle.
// Returns 0 outside, 1 straddling, 2 fully inside -- and 2 is the state
// ScenerySectionHeader::TreeCull (0x80209724) hands down to a node's children
// so they are never tested again. Four planes here rather than six, because
// near and far are already bounded by world.c's DRAW_RADIUS distance test, the
// same four SetupSceneryCullInfoPlat (0x801d7280) caches.
static int boxState(const float lo[3], const float hi[3])
{
	int state = 2;
	for (int k = 0; k < 4; k++) {
		const float* p = s_cullPlanes[k];
		const float d =
			p[0] * (lo[0] + hi[0]) * 0.5f + p[1] * (lo[1] + hi[1]) * 0.5f +
			p[2] * (lo[2] + hi[2]) * 0.5f + p[3];
		const float r =
			fabsf(p[0]) * (hi[0] - lo[0]) * 0.5f +
			fabsf(p[1]) * (hi[1] - lo[1]) * 0.5f +
			fabsf(p[2]) * (hi[2] - lo[2]) * 0.5f;
		if (d + r < 0.0f)
			return 0;
		if (d - r < 0.0f)
			state = 1;
	}
	return state;
}

// ------------------------------------------------ per-instance scenery LOD
//
// TASKS.md #247. Retail chooses each scenery instance's level of detail every
// frame from how big it is on screen, not by how far its section is: Speed.elf
// DrawAScenery (0x80208fa4), whose player-view path is all of this --
//
//     R = SceneryInfo +0x3c + 6.0                (packed as `radius`)
//     v = instance translation - camera position
//     dot(v, camera forward) < -R           ->  not drawn (behind the camera)
//     d = |v|  (0 when |v|^2 <= 5e-11)
//     P = (int)(R * H / (d - R))  if d - R > R, else (int)H
//     P <= 16                               ->  not drawn
//     A if A.polys <= 39, or A.volume > 0 and (float)P / max(A.density, 6)
//       >= 9.5; else B, and nothing if the B slot is empty
//
// with H = 256 / tan(fov / 2) from the camera's own field of view
// (CalculateH 0x801d8bf8, fed camera +0xc4 by CreateViewMatricies). The A
// model's three numbers are folded into `density` at pack time; the rest is
// here. Its frustum test is the instance's box, and only when its parent
// straddles the view, as GetVisibleState is only asked then.
//
// It replaces world.c's LOD_SWITCH, which was the port's own number: one
// switch per section at 40 m, with the far tier preferring the Z model --
// the reflection-view level retail never draws in a player view.
static bool  s_lodOn = false;
static float s_lodEye[3];
static float s_lodFwd[3];
static float s_lodH = 0.0f;

#define LOD_P_MIN        16      // P at or below this is not drawn
#define LOD_RATIO_A      9.5f    // 0x80338818
#define LOD_D2_MIN       5e-11f  // 0x80338808, below which d reads 0

void model3d_set_lod_view(const float eye[3], const float fwd[3], float H)
{
	s_lodFrame++;
	if (!eye || !fwd) {
		s_lodOn = false;
		return;
	}
	memcpy(s_lodEye, eye, sizeof(s_lodEye));
	memcpy(s_lodFwd, fwd, sizeof(s_lodFwd));
	s_lodH = H;
	s_lodOn = true;
}

// g_WorldLodLevel (#326). SPEED2.EXE's scenery pick (FUN_005D6570, the PC's
// DrawAScenery) takes an instance's A model (+0x30) only when FUN_005BE1C0
// reads 2, worldLod 2 or 3; at 0 and 1 every instance in the player's view is
// its B (+0x34), however near. The env and reflection views' picks do not read
// it. (0x883DDC, which FUN_005D2850 sets from the same value, is read nowhere.)
static bool s_lodAllowA = true;

void model3d_set_world_lod(int level)
{
	const bool allowA = level >= 2;
	if (allowA != s_lodAllowA)
		s_lodFrame++;
	s_lodAllowA = allowA;
}

// The env faces' pass of DrawAScenery (#166): the same P with the face
// camera's H, one threshold and no A/B choice -- the Z model (or B, for a
// 0x400 instance) at P >= 32. track_pack.py has already resolved which model
// that is and where it sits (ModelInstance::env).
static bool s_envView = false;

// model3d_draw_instance()'s draw of one instance on its own: the instance, the
// level it is drawn at and the batches holding it. -1: an ordinary draw.
static s32 s_solo = -1;
static u8 s_soloNeed = 0;
static const u8* s_soloMask = NULL;
#define ENV_P_MIN        32
// The pick's threshold in force: ENV_P_MIN, or DbgEnvPMin's (the env
// faces' 3DS cost, measured against what they add to the car).
// PORT (#432): 64 on the 3DS -- instances under ~16 px of a 64 px face
// are left out, not ~8. P 128 was pixel-identical at D24 and cut J29.1's
// env triangles 15%; 64 sits between. DbgEnvPMin=32 is the GameCube's.
#define ENV_P_MIN_3DS    64
static int s_envPMin = ENV_P_MIN_3DS;

void model3d_set_env_pmin(int p)
{
	s_envPMin = p > 0 ? p : ENV_P_MIN_3DS;
	s_lodFrame++;
}

// PORT (#432): an env frame's faces share one eye and one H, so the
// distance half of envPick() (and a model's reach) is the same for every
// face: it is worked out once per eye into the second half of envState and
// each face does only its own half-space and box tests. s_envDistGen moves
// on whenever what that half reads changes -- the eye, H, P_MIN, or a
// reflection view's terrain rule.
static bool  s_envPickOnce = true;
static u32   s_envDistGen = 1;
static float s_envDistKey[6];

void model3d_set_env_pick_once(bool on)
{
	s_envPickOnce = on;
	s_lodFrame++;
}

void model3d_set_env_view(bool on)
{
	s_envView = on;
	s_lodFrame++;
}

// The world origin (model3d.h, TASKS.md #376-#378, #382, #387): while it is
// set, drawBakedLod() subtracts it from every position in the shader
// (posoff) and folds it back into each matrix it uploads here, in 32-bit
// floats, so the clip position is the same and the shader's dp4 works on
// numbers near the camera.
static bool  s_originOn = false;
static float s_origin[3];

void model3d_set_world_origin(const float eye[3])
{
	s_originOn = eye != NULL;
	if (!eye)
		return;
	// Whole metres: exact in the PICA200's 24-bit floats (16-bit mantissa)
	// out to 65 km, so the shader's subtraction from a vertex -- itself on
	// that grid -- is exact too.
	for (int i = 0; i < 3; i++)
		s_origin[i] = floorf(eye[i] + 0.5f);
}

// `in` times the translation to the origin: column 3 gains the other three
// columns times it. The identity on `in` when no origin is set.
static void originFold(C3D_Mtx* out, const C3D_Mtx* in)
{
	*out = *in;
	if (!s_originOn)
		return;
	for (int i = 0; i < 4; i++)
		out->r[i].w = in->r[i].w + in->r[i].x * s_origin[0] + in->r[i].y * s_origin[1] +
			in->r[i].z * s_origin[2];
}

// A reflection view (#296, model3d.h): an env view less the terrain, each
// instance drawn through s_reflVp times its mirror. The mirror bound now is
// s_reflRow, uploaded as s_reflVp * W with W the identity but for its z row;
// drawBakedLod() puts the plain matrix up, which is the row (0, 0, 1, 0).
static bool    s_reflView = false;
static C3D_Mtx s_reflVp;
static float   s_reflRow[4];

void model3d_set_refl_view(const C3D_Mtx* vp)
{
	s_reflView = vp != NULL;
	if (vp)
		s_reflVp = *vp;
	s_envView = s_reflView;
	s_lodFrame++;
}

void model3d_lod_stats(u32* outA, u32* outB)
{
	if (outA) *outA = s_lodDrawnA;
	if (outB) *outB = s_lodDrawnB;
}

// DrawAScenery's pick for one instance: INST_DRAW_A, INST_DRAW_B or 0.
static u8 instancePick(const ModelInstance* in)
{
	// A part with no scenery record behind it has no LOD to pick and is drawn
	// whenever it is in view; it was baked into the base run. With no view
	// set (the DbgNearLod switch) every instance is its A.
	if (in->radius < 0.0f)
		return INST_DRAW_A | INST_DRAW_B;
	// Only in a reflection: no run of it is the player view's.
	if (in->env & INST_ENV_ONLY)
		return 0;
	if (!s_lodOn)
		return INST_DRAW_A;
	const float R = in->radius;
	const float vx = in->pos[0] - s_lodEye[0];
	const float vy = in->pos[1] - s_lodEye[1];
	const float vz = in->pos[2] - s_lodEye[2];
	// fcmpu + bge: a NaN falls through to "behind" as well.
	if (!(vx * s_lodFwd[0] + vy * s_lodFwd[1] + vz * s_lodFwd[2] >= -R))
		return 0;
	const float d2 = vx * vx + vy * vy + vz * vz;
	const float d = d2 > LOD_D2_MIN ? sqrtf(d2) : 0.0f;
	const float dr = d - R;
	const int P = (int)(dr > R ? R * s_lodH / dr : s_lodH);
	if (P <= LOD_P_MIN)
		return 0;
	if (!s_lodAllowA || in->density < 0.0f)
		return INST_DRAW_B;
	if (in->density == 0.0f)
		return INST_DRAW_A;
	return ((float)P / in->density >= LOD_RATIO_A) ? INST_DRAW_A : INST_DRAW_B;
}

// The env view's pick for one instance: INST_DRAW_B (its base or far run is
// the env model), INST_DRAW_E (its env-tier run) or 0.
static u8 envPick(const ModelInstance* in)
{
	const u32 kind = in->env & INST_ENV_KIND;
	if (!kind || in->radius < 0.0f)
		return 0;
	// StuffScenery's 8 in a reflection view: no terrain.
	if (s_reflView && (in->env & INST_ENV_NOREFL))
		return 0;
	const float R = in->radius;
	const float vx = in->pos[0] - s_lodEye[0];
	const float vy = in->pos[1] - s_lodEye[1];
	const float vz = in->pos[2] - s_lodEye[2];
	if (!(vx * s_lodFwd[0] + vy * s_lodFwd[1] + vz * s_lodFwd[2] >= -R))
		return 0;
	const float d2 = vx * vx + vy * vy + vz * vz;
	const float d = d2 > LOD_D2_MIN ? sqrtf(d2) : 0.0f;
	const float dr = d - R;
	const int P = (int)(dr > R ? R * s_lodH / dr : s_lodH);
	if (P < s_envPMin)
		return 0;
	return kind == INST_ENV_REUSE ? INST_DRAW_B : INST_DRAW_E;
}

// envPick()'s distance half: what every face of one eye shares.
static u8 envPickDist(const ModelInstance* in)
{
	const u32 kind = in->env & INST_ENV_KIND;
	if (!kind || in->radius < 0.0f)
		return 0;
	if (s_reflView && (in->env & INST_ENV_NOREFL))
		return 0;
	const float R = in->radius;
	const float vx = in->pos[0] - s_lodEye[0];
	const float vy = in->pos[1] - s_lodEye[1];
	const float vz = in->pos[2] - s_lodEye[2];
	const float d2 = vx * vx + vy * vy + vz * vz;
	const float d = d2 > LOD_D2_MIN ? sqrtf(d2) : 0.0f;
	const float dr = d - R;
	const int P = (int)(dr > R ? R * s_lodH / dr : s_lodH);
	if (P < s_envPMin)
		return 0;
	return kind == INST_ENV_REUSE ? INST_DRAW_B : INST_DRAW_E;
}

// The face's half: in front of its plane (envPick()'s first test).
static inline bool envPickAhead(const ModelInstance* in)
{
	const float vx = in->pos[0] - s_lodEye[0];
	const float vy = in->pos[1] - s_lodEye[1];
	const float vz = in->pos[2] - s_lodEye[2];
	return vx * s_lodFwd[0] + vy * s_lodFwd[1] + vz * s_lodFwd[2] >= -in->radius;
}

static void envDistSync(void)
{
	const float k[6] = { s_lodEye[0], s_lodEye[1], s_lodEye[2], s_lodH,
		(float)s_envPMin, s_reflView ? 1.0f : 0.0f };
	if (memcmp(k, s_envDistKey, sizeof(k)) != 0) {
		memcpy(s_envDistKey, k, sizeof(k));
		s_envDistGen++;
	}
}

// The state array the view being drawn reads: the env view's or the player's.
static inline const u8* viewState(const Model3D* m)
{
	return s_envView ? m->envState : m->instState;
}

static inline u8 viewAny(const Model3D* m)
{
	return s_envView ? m->envAny : m->instAny;
}

// This frame's state for every instance of `m`, once a frame. The model's
// own box stands in for TreeCull's parent node: wholly inside and no instance
// needs a test of its own, which in a section in front of the camera is most
// of them.
static void updateInstances(Model3D* m)
{
	if (s_envView) {
		if (!m->envState || m->envFrame == s_lodFrame)
			return;
		m->envFrame = s_lodFrame;
		if (s_envPickOnce) {
			u8* dist = m->envState + m->numInsts;
			envDistSync();
			if (m->envDistGen != s_envDistGen) {
				m->envDistGen = s_envDistGen;
				const float reach = m->envMaxR * (1.0f + s_lodH / (float)s_envPMin);
				float d2 = 0.0f;
				for (int k = 0; k < 3; k++) {
					const float e = s_lodEye[k];
					const float v = e < m->envLo[k] ? m->envLo[k] - e
						: e > m->envHi[k] ? e - m->envHi[k] : 0.0f;
					d2 += v * v;
				}
				u8 dany = 0;
				if (d2 <= reach * reach) {
					for (u32 i = 0; i < m->numInsts; i++) {
						dist[i] = envPickDist(&m->insts[i]);
						dany |= dist[i];
					}
				}
				m->envDistAny = dany;
			}
			if (!m->envDistAny) {
				if (m->envAny)
					memset(m->envState, 0, m->numInsts);
				m->envAny = 0;
				return;
			}
			float bbmax[3];
			for (int k = 0; k < 3; k++)
				bbmax[k] = m->center[k] * 2.0f - m->bbmin[k];
			const int parent = s_cullOn ? boxState(m->bbmin, bbmax) : 2;
			u8 any = 0;
			for (u32 i = 0; i < m->numInsts; i++) {
				const ModelInstance* in = &m->insts[i];
				u8 st = parent ? dist[i] : 0;
				if (st && !envPickAhead(in))
					st = 0;
				if (st) {
					const int fs = parent == 1 ? boxState(in->lo, in->hi) : 2;
					if (!fs)
						st = 0;
					else if (fs == 1)
						st |= INST_STRADDLE;
				}
				if (m->hidden && (m->hidden[i >> 3] & (1u << (i & 7))))
					st = 0;
				m->envState[i] = st;
				any |= st;
			}
			m->envAny = any;
			return;
		}
		// PORT: the picks are envPick's, but worked out cheapest first. P >=
		// ENV_P_MIN is d <= R (1 + H / ENV_P_MIN), so a model whose env
		// instances all sit further than that from the eye has none to draw,
		// and an instance is only box-tested once its distance passes. Retail
		// tests the box first; the pick is the same either way. (It saves
		// little: a face's cost is its draws, ~0.5 ms per 1,000 triangles on
		// Azahar, not these tests.)
		const float reach = m->envMaxR * (1.0f + s_lodH / (float)s_envPMin);
		float d2 = 0.0f;
		for (int k = 0; k < 3; k++) {
			const float e = s_lodEye[k];
			const float v = e < m->envLo[k] ? m->envLo[k] - e
				: e > m->envHi[k] ? e - m->envHi[k] : 0.0f;
			d2 += v * v;
		}
		if (d2 > reach * reach) {
			if (m->envAny)
				memset(m->envState, 0, m->numInsts);
			m->envAny = 0;
			return;
		}
		float bbmax[3];
		for (int k = 0; k < 3; k++)
			bbmax[k] = m->center[k] * 2.0f - m->bbmin[k];
		const int parent = s_cullOn ? boxState(m->bbmin, bbmax) : 2;
		u8 any = 0;
		for (u32 i = 0; i < m->numInsts; i++) {
			const ModelInstance* in = &m->insts[i];
			u8 st = 0;
			if (parent && (in->env & INST_ENV_KIND)) {
				st = envPick(in);
				if (st) {
					const int fs = parent == 1 ? boxState(in->lo, in->hi) : 2;
					if (!fs)
						st = 0;
					else if (fs == 1)
						st |= INST_STRADDLE;
				}
			}
			if (m->hidden && (m->hidden[i >> 3] & (1u << (i & 7))))
				st = 0;
			m->envState[i] = st;
			any |= st;
		}
		m->envAny = any;
		return;
	}
	if (m->instFrame == s_lodFrame)
		return;
	m->instFrame = s_lodFrame;
	const u64 t0 = s_cpuSplit ? svcGetSystemTick() : 0;
	float bbmax[3];
	for (int k = 0; k < 3; k++)
		bbmax[k] = m->center[k] * 2.0f - m->bbmin[k];
	const int parent = s_cullOn ? boxState(m->bbmin, bbmax) : 2;
	u8 any = 0;
	for (u32 i = 0; i < m->numInsts; i++) {
		const ModelInstance* in = &m->insts[i];
		int fs = parent;
		if (fs == 1)
			fs = boxState(in->lo, in->hi);
		u8 st = fs ? instancePick(in) : 0;
		if (st && fs == 1)
			st |= INST_STRADDLE;
		if (m->hidden && (m->hidden[i >> 3] & (1u << (i & 7))))
			st = 0;
		m->instState[i] = st;
		any |= st;
		if (st & INST_DRAW_A)
			s_lodDrawnA++;
		else if (st & INST_DRAW_B)
			s_lodDrawnB++;
	}
	m->instAny = any;
	if (s_cpuSplit)
		s_cpuInst += svcGetSystemTick() - t0;
}

// The INST_* level a batch's runs are drawn for, by where it sits in the list.
static inline u8 batchNeed(const Model3D* m, int i)
{
	return i < m->lodNearFirst ? (INST_DRAW_A | INST_DRAW_B)
	     : i < m->lodFarFirst  ? INST_DRAW_A
	     : i < m->envFirst     ? INST_DRAW_B : INST_DRAW_E;
}

static inline void submitRange(const Model3D* m, u32 first, u32 count)
{
	if (!count)
		return;
	drawElems((int)count,
		m->indices + first);
	s_drawCalls++;
	s_drawTris += count / 3;
}

// The mirror a reflection view draws instance `in` through: its NFSR row, the
// plain matrix for one with none (0x100), or about its own height.
static inline void reflRowFor(const Model3D* m, const ModelInstance* in, float row[4])
{
	const u32 n = in->env >> INST_ENV_ROW_SHIFT;
	if (n) {
		memcpy(row, m->reflRows + (n - 1) * 4, 4 * sizeof(float));
		return;
	}
	const bool flat = (in->env & INST_ENV_FLAT) != 0;
	row[0] = 0.0f;
	row[1] = 0.0f;
	row[2] = flat ? 1.0f : -1.0f;
	row[3] = flat ? 0.0f : 2.0f * in->pos[2];
}

// PORT: retail draws each prop through its own matrix; here a batch's props
// are one pool, and every change of mirror is a uniform upload and a new
// draw call. Two upright ones whose heights are within 5 cm share one -- a
// reflection 10 cm off, in a texture a quarter of the screen's size.
#define REFL_ROW_TOL 0.1f

static inline bool reflSame(const float row[4])
{
	const float* b = s_reflRow;
	if (row[0] == 0.0f && row[1] == 0.0f && row[2] == -1.0f &&
	    b[0] == 0.0f && b[1] == 0.0f && b[2] == -1.0f)
		return fabsf(row[3] - b[3]) < REFL_ROW_TOL;
	return memcmp(row, b, 4 * sizeof(float)) == 0;
}

// s_reflVp times W, W the identity with `row` for its z row: column j of the
// product is vp's column j plus vp's z column times row[j], z's is z times
// row[2].
static void reflBind(const float row[4])
{
	C3D_Mtx w;
	for (int i = 0; i < 4; i++) {
		const C3D_FVec* v = &s_reflVp.r[i];
		w.r[i].x = v->x + v->z * row[0];
		w.r[i].y = v->y + v->z * row[1];
		w.r[i].z = v->z * row[2];
		w.r[i].w = v->w + v->z * row[3];
	}
	C3D_Mtx wo;
	originFold(&wo, &w);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_sceneShader.uMvp, &wo);
	memcpy(s_reflRow, row, sizeof(s_reflRow));
	s_reflMirrors++;
}

// Submits the runs of one batch whose instances want this level, each
// unbroken stretch of kept runs as one call -- track_pack.py lays a tier's
// instances out in Morton order so that neighbours on screen are neighbours
// in the pool. `state` is the batch box's own boxState(): wholly inside and
// no boxed piece needs testing either.
static void drawInstancedRuns(const Model3D* m, const ModelBatch* b, u8 need,
		int state)
{
	const ModelRun* r = m->runs + b->runFirst;
	const ModelRun* end = r + b->runCount;
	const u8* vs = viewState(m);
	u32 first = 0, count = 0;
	if (s_solo >= 0) {
		if (!(s_soloNeed & need))
			return;
		for (; r < end; r++)
			if (r->inst == (u32)s_solo)
				submitRange(m, r->first, (u32)r->tris * 3);
		return;
	}
	g_walk[MODEL3D_WALK_RUNS] += b->runCount;
	for (; r < end; r++) {
		const u8 st = vs[r->inst];
		bool keep = (st & need) != 0;
		if (keep && r->box != RUN_NO_BOX && (st & INST_STRADDLE) && state != 2) {
			g_walk[MODEL3D_WALK_RUNBOX]++;
			const RunBox* bx = &m->runBoxes[r->box];
			keep = boxState(bx->lo, bx->hi) != 0;
		}
		if (keep && (s_hlCull || s_hlSkip)) {
			// A boxed piece by its own box, the rest by their instance's,
			// the boxes the view's cull already trusts. One with neither is
			// the beam's. s_hlSkip keeps the complement (#412 one pass).
			bool in = true;
			if (r->box != RUN_NO_BOX) {
				const RunBox* bx = &m->runBoxes[r->box];
				in = hlBoxIn(bx->lo, bx->hi);
			} else if (m->insts) {
				const ModelInstance* ins = &m->insts[r->inst];
				in = hlBoxIn(ins->lo, ins->hi);
			}
			keep = s_hlSkip ? !in : in;
		}
		if (!keep) {
			submitRange(m, first, count);
			count = 0;
			continue;
		}
		if (s_reflView) {
			float row[4];
			reflRowFor(m, &m->insts[r->inst], row);
			if (!reflSame(row)) {
				submitRange(m, first, count);
				count = 0;
				reflBind(row);
			}
		}
		const u32 n = (u32)r->tris * 3;
		if (count && first + count == r->first) {
			count += n;
			continue;
		}
		submitRange(m, first, count);
		first = r->first;
		count = n;
	}
	submitRange(m, first, count);
}

// Submits one batch. `state` is what boxState() said about its own box: 2, or
// a model with no cells, draws it in one call exactly as before; 1 walks the
// cells and draws each run of kept ones as a single call, which it can because
// a batch's cells are laid out consecutively in the index pool.
static void drawBatchRuns(const Model3D* m, const ModelBatch* b, int state)
{
	// A batch with no cells is the beam's whole (s_hlCull draws it all).
	if (s_hlSkip && (!m->cells || !b->cellCount))
		return;
	if ((state == 2 && !s_hlCull && !s_hlSkip) || !m->cells || !b->cellCount) {
		drawElems((int)b->indexCount,
			m->indices + b->firstIndex);
		s_drawCalls++;
		s_drawTris += b->indexCount / 3;
		return;
	}
	const CullCell* cc = m->cells + b->cellFirst;
	u32 first = 0, count = 0;
	for (u32 i = 0; i < b->cellCount; i++) {
		if ((state == 2 || boxState(cc[i].lo, cc[i].hi)) &&
		    (!s_hlCull || hlBoxIn(cc[i].lo, cc[i].hi)) &&
		    (!s_hlSkip || !hlBoxIn(cc[i].lo, cc[i].hi))) {
			if (!count)
				first = cc[i].first;
			count += cc[i].count;
			continue;
		}
		if (count) {
			drawElems((int)count,
				m->indices + first);
			s_drawCalls++;
			s_drawTris += count / 3;
		}
		count = 0;
	}
	if (count) {
		drawElems((int)count,
			m->indices + first);
		s_drawCalls++;
		s_drawTris += count / 3;
	}
}

// A car draw that moves its wheels (s_wh, below) puts up each corner part's
// own transform as its batches come, in every pass.
static bool s_whOn = false;
static void wheelBind(const ModelBatch* b);

// One batch's geometry, by whichever leaves the model has: an instance table's
// runs, or the grid cells (or the batch whole).
static void drawBatchGeometry(const Model3D* m, const ModelBatch* b, u8 need,
		int state)
{
	if (s_whOn)
		wheelBind(b);
	if (m->runs && b->runCount)
		drawInstancedRuns(m, b, need, state);
	else
		drawBatchRuns(m, b, state);
}

// The car LOD drawBatchPass() draws a carLods model at, for the length of one
// drawCarWith(), and the one model3d_set_car_lod() asked the next one for.
static int s_carLod = 0;
static int s_nextCarLod = 0;
// The rear rims model3d_draw_car_xform() found turned away, bit 0 corner 2
// and bit 1 corner 3 (MODEL3D_BATCH_CORNER_*), for the length of one draw.
static u8 s_carRimSwap = 0;
int g_dbgNoRimSwap = 0;

void model3d_set_car_lods(Model3D* m, bool on) {
	if (m)
		m->carLods = on;
}

void model3d_set_car_bay(Model3D* m, bool on) {
	if (m)
		m->carBay = on;
}

void model3d_set_car_rims(Model3D* m, const float axis[2][3]) {
	if (!m)
		return;
	m->rimAxes = axis != NULL;
	if (axis)
		memcpy(m->rimAxis, axis, sizeof(m->rimAxis));
}

void model3d_set_car_pivots(Model3D* m, const float pivot[4][3]) {
	if (!m)
		return;
	m->wheelPivots = pivot != NULL;
	if (pivot)
		memcpy(m->wheelPivot, pivot, sizeof(m->wheelPivot));
}

bool model3d_car_pivots(const Model3D* m, float out[4][3]) {
	if (!m || !m->wheelPivots)
		return false;
	memcpy(out, m->wheelPivot, sizeof(m->wheelPivot));
	return true;
}

void model3d_set_car_lod(int lod) {
	s_nextCarLod = lod < 0 ? 0 : lod >= MODEL3D_CAR_LODS ? MODEL3D_CAR_LODS - 1 : lod;
}

// ------------------------------------------- the in-world car light (#166)
//
// carsh.v.pica: the GameCube's per-vertex harmonics pass
// (epCalculateSphericalHarmonicsPOS16, Speed.elf 0x801e1c90), drawn twice --
// once for ps_Model mode 7's diffuse half, once more, additively, for its
// specular half (see the shader for the split).
typedef struct {
	DVLE_s*         dvle;     // the shared blob's (shaders.h, #352)
	shaderProgram_s program;
	bool            ready, tried;
	int uMvp, uSh, uEye, uMatD0, uMatD1, uMatS0, uMatS1, uMatP, uGloss;
	// The reflection, carenv.v.pica (#309); uEnvm -1 leaves the cars
	// without it.
	DVLE_s*         envDvle;
	shaderProgram_s envProgram;
	int uEnvm, uEnvK0, uEnvK1;
	// Both over the opaque batches in one draw, carge.v.pica (#431); geReady
	// false leaves the two passes.
	DVLE_s*         geDvle;
	shaderProgram_s geProgram;
	bool            geReady;
} ShShader;
static ShShader s_shShader;

// carenv.v.pica reads its uniforms where model3d.c sets them for carsh, so
// both programs must have them, and their constants, on the same registers.
static bool envMatchesSh(const DVLE_s* sh, const DVLE_s* env, shaderInstance_s* shV,
		shaderInstance_s* envV) {
	static const char* const kShared[] = {
		"mvp", "eye", "matD0", "matD1", "matP", "envm", "envk0", "envk1",
	};
	for (size_t i = 0; i < sizeof(kShared) / sizeof(kShared[0]); i++) {
		const s8 a = shaderInstanceGetUniformLocation(shV, kShared[i]);
		if (a < 0 || a != shaderInstanceGetUniformLocation(envV, kShared[i]))
			return false;
	}
	if (sh->constTableSize != env->constTableSize)
		return false;
	for (u32 i = 0; i < sh->constTableSize; i++) {
		const DVLE_constEntry_s* a = &sh->constTableData[i];
		const DVLE_constEntry_s* b = &env->constTableData[i];
		if (a->type != b->type || a->id != b->id || memcmp(a->data, b->data, sizeof(a->data)))
			return false;
	}
	return true;
}

static void envInit(ShShader* sh) {
	sh->uEnvm = -1;
	sh->envDvle = shaders_get(SHADER_CARENV);
	if (!sh->envDvle)
		return;
	shaderProgramInit(&sh->envProgram);
	shaderProgramSetVsh(&sh->envProgram, sh->envDvle);
	if (!envMatchesSh(sh->dvle, sh->envDvle, sh->program.vertexShader,
			sh->envProgram.vertexShader)) {
		shaderProgramFree(&sh->envProgram);
		sh->envDvle = NULL;
		return;
	}
	shaderInstance_s* v = sh->program.vertexShader;
	sh->uEnvm  = shaderInstanceGetUniformLocation(v, "envm");
	sh->uEnvK0 = shaderInstanceGetUniformLocation(v, "envk0");
	sh->uEnvK1 = shaderInstanceGetUniformLocation(v, "envk1");
}

// carge.v.pica (#431) draws with the uniforms model3d.c sets for carsh, so it
// must have every one it reads on carsh's register, and carsh's constants.
static void geInit(ShShader* sh) {
	static const char* const kShared[] = {
		"mvp", "sh", "eye", "matD0", "matD1", "matS0", "matS1", "matP", "envm",
	};
	sh->geReady = false;
	if (sh->uEnvm < 0)
		return;
	sh->geDvle = shaders_get(SHADER_CARGE);
	if (!sh->geDvle)
		return;
	shaderProgramInit(&sh->geProgram);
	shaderProgramSetVsh(&sh->geProgram, sh->geDvle);
	shaderInstance_s* a = sh->program.vertexShader;
	shaderInstance_s* b = sh->geProgram.vertexShader;
	bool same = sh->dvle->constTableSize == sh->geDvle->constTableSize;
	for (size_t i = 0; same && i < sizeof(kShared) / sizeof(kShared[0]); i++) {
		const s8 x = shaderInstanceGetUniformLocation(a, kShared[i]);
		same = x >= 0 && x == shaderInstanceGetUniformLocation(b, kShared[i]);
	}
	for (u32 i = 0; same && i < sh->dvle->constTableSize; i++) {
		const DVLE_constEntry_s* p = &sh->dvle->constTableData[i];
		const DVLE_constEntry_s* q = &sh->geDvle->constTableData[i];
		same = p->type == q->type && p->id == q->id && !memcmp(p->data, q->data, sizeof(p->data));
	}
	if (!same) {
		shaderProgramFree(&sh->geProgram);
		sh->geDvle = NULL;
		dbglog_line(NULL, "carge: the program's uniforms are not carsh's -- gloss and reflection stay two passes");
		return;
	}
	sh->geReady = true;
}

static bool shInit(void) {
	ShShader* sh = &s_shShader;
	if (sh->tried)
		return sh->ready;
	sh->tried = true;
	sh->dvle = shaders_get(SHADER_CARSH);
	if (!sh->dvle)
		return false;
	shaderProgramInit(&sh->program);
	shaderProgramSetVsh(&sh->program, sh->dvle);
	shaderInstance_s* v = sh->program.vertexShader;
	sh->uMvp   = shaderInstanceGetUniformLocation(v, "mvp");
	sh->uSh    = shaderInstanceGetUniformLocation(v, "sh");
	sh->uEye   = shaderInstanceGetUniformLocation(v, "eye");
	sh->uMatD0 = shaderInstanceGetUniformLocation(v, "matD0");
	sh->uMatD1 = shaderInstanceGetUniformLocation(v, "matD1");
	sh->uMatS0 = shaderInstanceGetUniformLocation(v, "matS0");
	sh->uMatS1 = shaderInstanceGetUniformLocation(v, "matS1");
	sh->uMatP  = shaderInstanceGetUniformLocation(v, "matP");
	sh->uGloss = shaderInstanceGetUniformLocation(v, "gloss");
	if (sh->uMvp < 0 || sh->uSh < 0 || sh->uEye < 0 || sh->uMatD0 < 0 ||
			sh->uMatD1 < 0 || sh->uMatS0 < 0 || sh->uMatS1 < 0 || sh->uMatP < 0 ||
			sh->uGloss < 0) {
		shaderProgramFree(&sh->program);
		sh->dvle = NULL;
		return false;
	}
	envInit(sh);
	geInit(sh);
	sh->ready = true;
	return true;
}

bool model3d_set_light_mats(Model3D* m, const u8* mats, int n) {
	if (!m || !mats || n != m->numBatches || !shInit())
		return false;
	u8* t = (u8*)malloc((size_t)n);
	if (!t)
		return false;
	memcpy(t, mats, (size_t)n);
	free(m->lightMat);
	m->lightMat = t;
	return true;
}

// #345: one draw call per run of batches that draw alike. An assembled car is
// a few dozen parts, each with its own batches, and drawBatchPass() puts every
// batch through citro3d's per-draw state flush -- three times on the in-world
// car, whose opaque batches take a gloss and an env pass too. Neighbours in
// draw order whose texture slot, flags (paint slot, LOD, wheel corner and
// part) and light material all match are drawn with the same state, so their
// index ranges are laid end to end and drawn as one. The draw order and every
// state the loop sets are what they were, so the picture is too. Only for a
// model with nothing that keeps batch numbers or index ranges of its own: no
// LOD split, no env tier, no cull cells or runs, no collision cutoff.
int g_dbgNoMerge = 0;
// DbgNoMergeSort=1: the #345 merge alone, neighbours only (the A/B check).
int g_dbgNoMergeSort = 0;
int g_mergeStat[7];

static bool indexBox(const Model3D* m, u32 first, u32 count, float lo[3],
	float hi[3]);

static bool mergeSameState(const Model3D* m, int a, int b) {
	const ModelBatch* x = &m->batches[a];
	const ModelBatch* y = &m->batches[b];
	return x->texIndex == y->texIndex && x->flags == y->flags &&
		(!m->lightMat || m->lightMat[a] == m->lightMat[b]);
}

// Whether two opaque batches' draw order can show (#352). Opaque, depth-tested
// geometry ends up the same whatever order it is drawn in, except where two
// surfaces meet at one depth (GPU_GREATER keeps the first); the gloss and env
// passes over it add at GEQUAL, which is order-free. So only batches whose boxes
// meet are kept in order -- unless they are never drawn at one level of detail.
// A wheel moves (spin, steer, suspension) and its rim may stand in for another
// LOD's (s_carRimSwap), so its box is widened to the ball it can sweep and its
// LOD bits are not trusted.
#define MERGE_TOUCH 0.01f
static bool mergeOrderMatters(const Model3D* m, int a, int b) {
	const ModelBatch* x = &m->batches[a];
	const ModelBatch* y = &m->batches[b];
	// An opening part (#372 step 7) swings well past its box, and may be
	// left out of a draw altogether: kept in order with everything.
	if ((x->flags | y->flags) & MODEL3D_BATCH_PART)
		return true;
	const bool wx = (x->flags & MODEL3D_BATCH_WHEEL_MASK) != 0;
	const bool wy = (y->flags & MODEL3D_BATCH_WHEEL_MASK) != 0;
	if (!wx && !wy) {
		const u32 lx = (x->flags & MODEL3D_BATCH_LOD_MASK) >> MODEL3D_BATCH_LOD_SHIFT;
		const u32 ly = (y->flags & MODEL3D_BATCH_LOD_MASK) >> MODEL3D_BATCH_LOD_SHIFT;
		if (m->carLods && lx && ly && !(lx & ly))
			return false;
	}
	float lo[2][3], hi[2][3];
	const ModelBatch* bb[2] = { x, y };
	const bool wh[2] = { wx, wy };
	for (int s = 0; s < 2; s++) {
		float r = 0.0f;
		for (int c = 0; c < 3; c++) {
			const float h = 0.5f * (bb[s]->hi[c] - bb[s]->lo[c]);
			if (h > r) r = h;
		}
		for (int c = 0; c < 3; c++) {
			const float mid = 0.5f * (bb[s]->lo[c] + bb[s]->hi[c]);
			// A wheel: the ball about its middle, plus the suspension's travel.
			const float g = wh[s] ? 0.5f * r + 0.15f : 0.0f;
			lo[s][c] = (wh[s] ? mid - r : bb[s]->lo[c]) - g - MERGE_TOUCH;
			hi[s][c] = (wh[s] ? mid + r : bb[s]->hi[c]) + g + MERGE_TOUCH;
		}
	}
	for (int c = 0; c < 3; c++)
		if (lo[0][c] > hi[1][c] || lo[1][c] > hi[0][c])
			return false;
	return true;
}

// The draw order model3d_merge_batches() lays the batches out in: the blended
// slice as it was (its order is its look), and each opaque batch moved back to
// the last earlier one that draws alike, when nothing it would jump has an
// order with it that shows. Everything else stays where it was.
static void mergeOrder(const Model3D* m, int* seq) {
	const int n = m->numBatches;
	const int nOpq = m->passOrder ? m->numOpaque : 0;
	int* grp = NULL;       // a group's first member, in group order
	int* nxt = NULL;       // the next member of the same group, -1 for the last
	int* tail = NULL;
	bool ok = nOpq > 1 && !g_dbgNoMergeSort;
	if (ok) {
		grp = (int*)malloc((size_t)nOpq * sizeof(int));
		tail = (int*)malloc((size_t)nOpq * sizeof(int));
		nxt = (int*)malloc((size_t)n * sizeof(int));
		ok = grp && tail && nxt;
	}
	if (ok) {
		for (int k = 0; k < nOpq; k++) {
			ModelBatch* b = &m->batches[m->passOrder[k]];
			if (!b->indexCount) {   // draws nothing: any order
				memset(b->lo, 0, sizeof(b->lo));
				memset(b->hi, 0, sizeof(b->hi));
				continue;
			}
			if (!indexBox(m, b->firstIndex, b->indexCount, b->lo, b->hi)) {
				ok = false;
				break;
			}
		}
	}
	if (!ok) {
		free(grp);
		free(nxt);
		free(tail);
		for (int k = 0; k < n; k++)
			seq[k] = m->passOrder ? (int)m->passOrder[k] : k;
		return;
	}
	int ng = 0;
	for (int k = 0; k < nOpq; k++) {
		const int i = m->passOrder[k];
		nxt[i] = -1;
		int into = -1;
		for (int g = ng - 1; g >= 0; g--) {
			if (mergeSameState(m, grp[g], i)) {
				into = g;
				break;
			}
			bool shows = false;
			for (int j = grp[g]; j >= 0 && !shows; j = nxt[j])
				shows = mergeOrderMatters(m, j, i);
			if (shows)
				break;
		}
		if (into >= 0) {
			nxt[tail[into]] = i;
			tail[into] = i;
		} else {
			grp[ng] = tail[ng] = i;
			ng++;
		}
	}
	// What could merge at best (#352's log): distinct states, all told, and
	// with the LOD bits / the retail low flag bits left out.
	{
		int d[3] = { 0, 0, 0 };
		const u32 drop[3] = { 0, MODEL3D_BATCH_LOD_MASK, 0xFFFFu & ~0x100u };
		for (int v = 0; v < 3; v++)
			for (int k = 0; k < nOpq; k++) {
				const int i = m->passOrder[k];
				bool seen = false;
				for (int q = 0; q < k && !seen; q++) {
					const int j = m->passOrder[q];
					seen = m->batches[i].texIndex == m->batches[j].texIndex &&
						(m->batches[i].flags & ~drop[v]) == (m->batches[j].flags & ~drop[v]) &&
						(!m->lightMat || m->lightMat[i] == m->lightMat[j]);
				}
				d[v] += !seen;
			}
		// At LOD A: the groups drawn, and the states drawn.
		int ga = 0, sa = 0;
		for (int g = 0; g < ng; g++) {
			const u32 l = (m->batches[grp[g]].flags & MODEL3D_BATCH_LOD_MASK) >>
				MODEL3D_BATCH_LOD_SHIFT;
			ga += !l || (l & 1u);
		}
		for (int k = 0; k < nOpq; k++) {
			const int i = m->passOrder[k];
			const u32 l = (m->batches[i].flags & MODEL3D_BATCH_LOD_MASK) >>
				MODEL3D_BATCH_LOD_SHIFT;
			if (l && !(l & 1u))
				continue;
			bool seen = false;
			for (int q = 0; q < k && !seen; q++) {
				const int j = m->passOrder[q];
				const u32 lj = (m->batches[j].flags & MODEL3D_BATCH_LOD_MASK) >>
					MODEL3D_BATCH_LOD_SHIFT;
				if (lj && !(lj & 1u))
					continue;
				seen = m->batches[i].texIndex == m->batches[j].texIndex &&
					(m->batches[i].flags & ~MODEL3D_BATCH_LOD_MASK) ==
					(m->batches[j].flags & ~MODEL3D_BATCH_LOD_MASK) &&
					(!m->lightMat || m->lightMat[i] == m->lightMat[j]);
			}
			sa += !seen;
		}
		g_mergeStat[5] = ga;
		g_mergeStat[6] = sa;
		g_mergeStat[0] = nOpq;
		g_mergeStat[1] = ng;
		g_mergeStat[2] = d[0];
		g_mergeStat[3] = d[1];
		g_mergeStat[4] = d[2];
	}
	int at = 0;
	for (int g = 0; g < ng; g++)
		for (int j = grp[g]; j >= 0; j = nxt[j])
			seq[at++] = j;
	for (int k = nOpq; k < n; k++)
		seq[at++] = (int)m->passOrder[k];
	free(grp);
	free(nxt);
	free(tail);
}

int model3d_merge_batches(Model3D* m) {
	if (g_dbgNoMerge || !m || m->numBatches < 2 || m->cells || m->runs || m->physGrid ||
	    m->lodNearFirst != m->numBatches || m->lodFarFirst != m->numBatches ||
	    m->envFirst != m->numBatches || m->physIndices != m->numIndices ||
	    m->numBatches > 0xFFFF)
		return 0;
	const int n = m->numBatches;
	for (int i = 0; i < n; i++) {
		const ModelBatch* b = &m->batches[i];
		if (b->firstIndex + b->indexCount > (u32)m->numIndices)
			return 0;
	}
	ModelBatch* nb = (ModelBatch*)malloc((size_t)n * sizeof(ModelBatch));
	u8* nl = m->lightMat ? (u8*)malloc((size_t)n) : NULL;
	u16* idx = (u16*)malloc((size_t)m->numIndices * sizeof(u16));
	if (!nb || !idx || (m->lightMat && !nl)) {
		free(nb);
		free(nl);
		free(idx);
		return 0;
	}
	// #352: the opaque slice gathered by state first (mergeOrder), so batches
	// that draw alike but sit apart in file order still become one draw.
	int* seq = (int*)malloc((size_t)n * sizeof(int));
	if (!seq) {
		free(nb);
		free(nl);
		free(idx);
		return 0;
	}
	mergeOrder(m, seq);
	// The glass (#324): each window batch stays a batch of its own, so it can
	// take its own cracked pane; glassAt is where each one lands.
	s16* glassOf = NULL;
	u16* glassAt = NULL;
	if (m->numGlass) {
		glassOf = (s16*)malloc((size_t)n * sizeof(s16));
		glassAt = (u16*)malloc((size_t)m->numGlass * sizeof(u16));
		if (!glassOf || !glassAt) {
			free(glassOf);
			free(glassAt);
			free(seq);
			free(nb);
			free(nl);
			free(idx);
			return 0;
		}
		for (int i = 0; i < n; i++)
			glassOf[i] = -1;
		for (int g = 0; g < m->numGlass; g++)
			glassOf[m->glassBatch[g]] = (s16)g;
	}
	bool prevGlass = false;
	int out = 0, numOpaque = 0;
	u32 at = 0;
	for (int k = 0; k < n; k++) {
		const int i = seq[k];
		const ModelBatch* b = &m->batches[i];
		const u8 lm = m->lightMat ? m->lightMat[i] : 0;
		// Never across the opaque/blended boundary of the sorted list.
		const bool boundary = m->passOrder && k == m->numOpaque;
		if (at + b->indexCount > (u32)m->numIndices) {   // overlapping ranges
			free(glassOf);
			free(glassAt);
			free(seq);
			free(nb);
			free(nl);
			free(idx);
			return 0;
		}
		const int glass = glassOf ? glassOf[i] : -1;
		ModelBatch* p = out ? &nb[out - 1] : NULL;
		if (glass >= 0)
			glassAt[glass] = (u16)out;
		if (p && !boundary && glass < 0 && !prevGlass && p->texIndex == b->texIndex &&
		    p->flags == b->flags && (!nl || nl[out - 1] == lm)) {
			p->indexCount += b->indexCount;
			for (int c = 0; c < 3; c++) {
				if (b->lo[c] < p->lo[c]) p->lo[c] = b->lo[c];
				if (b->hi[c] > p->hi[c]) p->hi[c] = b->hi[c];
			}
		} else {
			nb[out] = *b;
			nb[out].firstIndex = at;
			nb[out].indexCount = b->indexCount;
			if (nl)
				nl[out] = lm;
			out++;
		}
		prevGlass = glass >= 0;
		memcpy(idx + at, m->indices + b->firstIndex, (size_t)b->indexCount * sizeof(u16));
		at += b->indexCount;
		if (m->passOrder && k + 1 == m->numOpaque)
			numOpaque = out;
	}
	free(seq);
	// Whatever no batch named is kept after the batches' ranges, so a reader
	// of the whole pool still sees every triangle.
	if (at < (u32)m->numIndices) {
		u8* used = (u8*)calloc((size_t)m->numIndices, 1);
		if (!used) {
			free(glassOf);
			free(glassAt);
			free(nb);
			free(nl);
			free(idx);
			return 0;
		}
		for (int i = 0; i < n; i++)
			memset(used + m->batches[i].firstIndex, 1, m->batches[i].indexCount);
		for (int j = 0; j < m->numIndices; j++)
			if (!used[j])
				idx[at++] = m->indices[j];
		free(used);
		if (at != (u32)m->numIndices) {   // overlapping ranges: leave it be
			free(glassOf);
			free(glassAt);
			free(nb);
			free(nl);
			free(idx);
			return 0;
		}
	}
	if (glassAt)
		memcpy(m->glassBatch, glassAt, (size_t)m->numGlass * sizeof(u16));
	free(glassOf);
	free(glassAt);
	const int before = n;
	memcpy(m->indices, idx, (size_t)m->numIndices * sizeof(u16));
	GSPGPU_FlushDataCache(m->indices, (u32)m->numIndices * sizeof(u16));
	free(idx);
	free(m->batches);
	m->batches = nb;
	if (nl) {
		free(m->lightMat);
		m->lightMat = nl;
	}
	m->numBatches = out;
	m->lodNearFirst = m->lodFarFirst = m->envFirst = out;
	if (m->passOrder) {
		for (int k = 0; k < out; k++)
			m->passOrder[k] = (u16)k;
		m->numOpaque = numOpaque;
	}
	return before - out;
}

bool model3d_set_glass(Model3D* m, const int* batch, const u8* win, int n,
		const s32 tex[3]) {
	if (!m || !batch || !win || n <= 0 || n > 0xFFFF || m->extTextures)
		return false;
	for (int k = 0; k < 3; k++)
		if (tex[k] < 0 || tex[k] >= m->numTextures || !m->texValid[tex[k]])
			return false;
	for (int g = 0; g < n; g++)
		if (batch[g] < 0 || batch[g] >= m->numBatches || win[g] >= MODEL3D_GLASS_WINDOWS)
			return false;
	u16* gb = (u16*)malloc((size_t)n * sizeof(u16));
	u8* gw = (u8*)malloc((size_t)n);
	s32* go = (s32*)malloc((size_t)n * sizeof(s32));
	if (!gb || !gw || !go) {
		free(gb);
		free(gw);
		free(go);
		return false;
	}
	for (int g = 0; g < n; g++) {
		gb[g] = (u16)batch[g];
		gw[g] = win[g];
		go[g] = m->batches[batch[g]].texIndex;
	}
	free(m->glassBatch);
	free(m->glassWin);
	free(m->glassOrig);
	m->glassBatch = gb;
	m->glassWin = gw;
	m->glassOrig = go;
	m->numGlass = n;
	memcpy(m->glassTex, tex, sizeof(m->glassTex));
	memset(m->glassLevel, 0, sizeof(m->glassLevel));
	return true;
}

int model3d_glass_count(const Model3D* m) {
	return m ? m->numGlass : 0;
}

void model3d_set_glass_levels(Model3D* m, const u8 level[MODEL3D_GLASS_WINDOWS]) {
	if (!m || !m->numGlass || !memcmp(level, m->glassLevel, sizeof(m->glassLevel)))
		return;
	memcpy(m->glassLevel, level, sizeof(m->glassLevel));
	for (int g = 0; g < m->numGlass; g++) {
		const u8 lv = level[m->glassWin[g]];
		m->batches[m->glassBatch[g]].texIndex = lv ? m->glassTex[(lv > 3 ? 3 : lv) - 1]
		                                            : m->glassOrig[g];
	}
}

static bool  s_carViewSet = false;
static float s_carEye[3], s_carRight[3], s_carDown[3], s_carFwd[3];

void model3d_set_car_view(const float eye[3], const float right[3],
		const float up[3], const float fwd[3]) {
	for (int i = 0; i < 3; i++) {
		s_carEye[i] = eye[i];
		s_carRight[i] = right[i];
		s_carDown[i] = -up[i];
		s_carFwd[i] = fwd[i];
	}
	s_carViewSet = true;
}

// The reflection every in-world car samples (envmap.c), and its camera's
// axes in the world.
static const C3D_Tex* s_carEnvTex = NULL;
static float s_carEnvAxes[3][3];

void model3d_set_car_env(const C3D_Tex* sphere, const float right[3], const float up[3],
		const float back[3]) {
	s_carEnvTex = sphere;
	if (!sphere)
		return;
	for (int i = 0; i < 3; i++) {
		s_carEnvAxes[0][i] = right[i];
		s_carEnvAxes[1][i] = up[i];
		s_carEnvAxes[2][i] = back[i];
	}
}

static Model3DCarLightQuery s_carLightFn = NULL;
static void* s_carLightUd = NULL;

void model3d_set_car_light_query(Model3DCarLightQuery fn, void* ud) {
	s_carLightFn = fn;
	s_carLightUd = ud;
}

// ShaperLightsCarsInGame, the rig every car in the world gets (elSetupLights,
// 0x800cb290, from CarRenderInfo::Render): records 1-3 of the .bss rig the
// static initializer fills (0x800d064c..). All three are white and
// camera-relative.
static const Model3DShaperLight kCarsInGame[MODEL3D_RIG_LIGHTS] = {
	{ 1, 200.0f, 90.0f, { 1.0f, 1.0f, 1.0f }, 0.5f },
	{ 1,  42.0f, 33.0f, { 1.0f, 1.0f, 1.0f }, 0.35f },
	{ 1, 219.0f, 10.0f, { 1.0f, 1.0f, 1.0f }, 0.1f },
};

// The rig model3d_set_car_rig() last set, and elSetupEnvMap's IN_FRONTEND.
static Model3DShaperLight s_carRig[MODEL3D_RIG_LIGHTS] = {
	{ 1, 200.0f, 90.0f, { 1.0f, 1.0f, 1.0f }, 0.5f },
	{ 1,  42.0f, 33.0f, { 1.0f, 1.0f, 1.0f }, 0.35f },
	{ 1, 219.0f, 10.0f, { 1.0f, 1.0f, 1.0f }, 0.1f },
};
static bool s_carRigFe = false;

void model3d_set_car_rig(const Model3DShaperLight* rig, bool frontEnd) {
	memcpy(s_carRig, rig ? rig : kCarsInGame, sizeof(s_carRig));
	s_carRigFe = frontEnd;
}

// Irradiance::UpdateCoeffs (0x800ca1f4): one light of colour `col` * s from
// unit direction d into the ten quadratic coefficients, each RGBA. The yz
// term takes c1 where the others take 2 c1, as retail's does.
static void shUpdate(float c[10][4], const float d[3], const float col[4], float s) {
	const float c1 = 0.429043f, c2 = 0.511664f, c3 = 0.743125f, c4 = 0.886227f,
	            c5 = 0.247708f;
	const float x = d[0], y = d[1], z = d[2];
	for (int ch = 0; ch < 4; ch++) {
		const float f = col[ch] * s;
		const float l00 = 0.282095f * f;
		const float l2m2 = 1.092548f * f * x * y;
		const float l2m1 = 1.092548f * f * y * z;
		const float l21 = 1.092548f * f * x * z;
		const float l20 = 0.315392f * f * (3.0f * z * z - 1.0f);
		const float l22 = 0.546274f * f * (x * x - y * y);
		c[0][ch] += c4 * l00 - c5 * l20;
		c[1][ch] += c1 * l22;
		c[2][ch] -= c1 * l22;
		c[3][ch] += c3 * l20;
		c[4][ch] += 2.0f * c1 * l2m2;
		c[5][ch] += 2.0f * c1 * l21;
		c[6][ch] += c1 * l2m1;
		c[7][ch] += 2.0f * c2 * 0.488603f * f * x;
		c[8][ch] += 2.0f * c2 * 0.488603f * f * y;
		c[9][ch] += 2.0f * c2 * 0.488603f * f * z;
	}
}

// UpdateByDirectionalLight (0x800ca454): the light from unit direction `d`,
// and a twentieth of it negated from the far side.
static void shLight(float c[10][4], const float d[3], const float col[4]) {
	const float nd[3] = { -d[0], -d[1], -d[2] };
	shUpdate(c, d, col, 1.0f);
	shUpdate(c, nd, col, -1.0f / 20.0f);
}

// The car's light context in the world: elSetupLights for the rig and the
// lights QueryLightDatabase finds around the car at `origin`.
static void carLightWorld(const float origin[3], float c[10][4]) {
	memset(c, 0, sizeof(float) * 10 * 4);
	for (int k = 0; k < MODEL3D_RIG_LIGHTS; k++) {
		const Model3DShaperLight* R = &s_carRig[k];
		// SphericalToCartesian(az + 180, 90 - el). A camera-relative record
		// then takes (-y, -z, x) * iR: the rig's direction in the camera's
		// (right, down, forward), turned into the world by the inverse camera
		// rotation. A world one is a world direction already.
		const float th = (R->az + 180.0f) * ((float)M_PI / 180.0f);
		const float ph = (90.0f - R->el) * ((float)M_PI / 180.0f);
		const float d[3] = { cosf(th) * sinf(ph), sinf(th) * sinf(ph), cosf(ph) };
		float w[3];
		if (R->camera) {
			const float v[3] = { -d[1], -d[2], d[0] };
			for (int i = 0; i < 3; i++)
				w[i] = v[0] * s_carRight[i] + v[1] * s_carDown[i] + v[2] * s_carFwd[i];
		} else {
			memcpy(w, d, sizeof(w));
		}
		const float wl = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
		if (wl < 1e-6f)
			continue;
		for (int i = 0; i < 3; i++)
			w[i] /= wl;
		// The record's colour at its intensity; alpha is 0.29 R + 0.54 G +
		// 0.17 B.
		float col[4];
		for (int i = 0; i < 3; i++)
			col[i] = R->rgb[i] * R->intensity;
		col[3] = 0.29f * col[0] + 0.54f * col[1] + 0.17f * col[2];
		shLight(c, w, col);
	}
	// Then the database's lights, each from where it stands as seen from the
	// car (L.pos - pos), its colour over 255 and alpha its luminance. With
	// more than 16 found the count becomes 15 (sic: n + 3 > 19).
	if (s_carLightFn) {
		Model3DCarLight L[MODEL3D_CAR_LIGHTS];
		int n = s_carLightFn(origin, L, MODEL3D_CAR_LIGHTS, s_carLightUd);
		if (n + 3 > 19)
			n = 15;
		for (int i = 0; i < n; i++) {
			float d[3] = { L[i].pos[0] - origin[0], L[i].pos[1] - origin[1],
			               L[i].pos[2] - origin[2] };
			const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			for (int a = 0; a < 3; a++)
				d[a] = dl > 0.0f ? d[a] / dl : 0.0f;
			float col[4];
			for (int a = 0; a < 3; a++)
				col[a] = L[i].rgb[a] * (1.0f / 255.0f);
			col[3] = 0.29f * col[0] + 0.54f * col[1] + 0.17f * col[2];
			shLight(c, d, col);
		}
	}
	// Normalize(1.0, 0.8, normRGB 0, normA 1): RGB as it is, alpha to an L2
	// norm of 0.8.
	float n2 = 0.0f;
	for (int k = 0; k < 10; k++)
		n2 += c[k][3] * c[k][3];
	if (n2 > 1e-20f) {
		const float s = 0.8f / sqrtf(n2);
		for (int k = 0; k < 10; k++)
			c[k][3] *= s;
	}
}

// That context in a frame's model space, elCloneLightContext's rotation
// (Irradiance::Transform, 0x800ca530), and elSetupEnvMap's eye (0x800cb054):
// the car's own, or one of its wheels' (CarRenderInfo::Render clones it per
// wheel). `rot` has the frame's axes in world space as rows, `origin` is its
// origin in the world.
static void carLightFrame(const float c[10][4], const float rot[3][3],
		const float origin[3], float out[10][4], float eye[3]) {
	// Into model space, as a quadratic form: p'(n) = p(Rot n), A' = Rot' A
	// Rot, b' = Rot' b, where Rot's column a is rot[a].
	for (int ch = 0; ch < 4; ch++) {
		const float A[3][3] = {
			{ c[1][ch],        c[4][ch] * 0.5f, c[5][ch] * 0.5f },
			{ c[4][ch] * 0.5f, c[2][ch],        c[6][ch] * 0.5f },
			{ c[5][ch] * 0.5f, c[6][ch] * 0.5f, c[3][ch] },
		};
		float RA[3][3];   // rot[a]' A
		for (int a = 0; a < 3; a++)
			for (int j = 0; j < 3; j++)
				RA[a][j] = rot[a][0] * A[0][j] + rot[a][1] * A[1][j] + rot[a][2] * A[2][j];
		float Ap[3][3];
		for (int a = 0; a < 3; a++)
			for (int b = 0; b < 3; b++)
				Ap[a][b] = RA[a][0] * rot[b][0] + RA[a][1] * rot[b][1] + RA[a][2] * rot[b][2];
		out[0][ch] = c[0][ch];
		out[1][ch] = Ap[0][0];
		out[2][ch] = Ap[1][1];
		out[3][ch] = Ap[2][2];
		out[4][ch] = Ap[0][1] * 2.0f;
		out[5][ch] = Ap[0][2] * 2.0f;
		out[6][ch] = Ap[1][2] * 2.0f;
		for (int a = 0; a < 3; a++)
			out[7 + a][ch] = rot[a][0] * c[7][ch] + rot[a][1] * c[8][ch] +
			                 rot[a][2] * c[9][ch];
	}
	// elSetupEnvMap: the camera in model space at a length of sqrt(|l|^2 + 1)
	// clamped to 2.78..2.78 in the world -- so always 2.78 -- and to 2.78..8
	// in the frontend.
	float l[3];
	for (int a = 0; a < 3; a++)
		l[a] = rot[a][0] * (s_carEye[0] - origin[0]) + rot[a][1] * (s_carEye[1] - origin[1]) +
		       rot[a][2] * (s_carEye[2] - origin[2]);
	const float l2 = l[0] * l[0] + l[1] * l[1] + l[2] * l[2];
	const float ll = sqrtf(l2);
	float len = sqrtf(l2 + 1.0f);
	const float hi = s_carRigFe ? 8.0f : 2.78f;
	len = len < 2.78f ? 2.78f : len > hi ? hi : len;
	for (int a = 0; a < 3; a++)
		eye[a] = ll > 1e-6f ? l[a] / ll * len : (a == 2 ? len : 0.0f);
}

// ------------------------------------------------ the wheels' motion (#296)
//
// CarState::CalculateWheelMatrices (Speed.elf 0x801aa974) gives each corner a
// matrix: RotY by the spin, then RotZ by the steering as drawn at the front,
// at the wheel record's position lifted by the suspension; the brakes the
// same without the spin, the spinners (GetRenderingSpinnerMatrices,
// 0x80223c44) their own angle in its place. CarRenderInfo::Render (0x80239018,
// SPEED2.EXE 0x620ae0) turns the front ones by the camber about x at the
// car's LOD A and B (+ on FL, - on FR) and clones the light context into
// each. The rims are baked in place here (carkit.c), so a corner part's
// batches are drawn by D = T(pivot + lift) R T(-pivot), R = Rx Rz Ry.

// One frame a car's batches are drawn in: the body's (0), a corner part's
// (4..15: MODEL3D_BATCH_CORNER / _WHEEL bits), or an opening part's (16..20:
// PART_FRAME0 + MODEL3D_PART_FRAMES, #372 step 7). `rot` and `origin` as
// carLightFrame takes them; `sh`, `eye` and `envm` for carsh, `normal` for
// car.v.pica.
typedef struct {
	C3D_Mtx mvp, normal;
	float rot[3][3], origin[3];
	float sh[10][4], eye[3], envm[3][3];
} WheelFrame;
#define PART_FRAME0  16
// The engine's and the trunk install's scaled light (#314,
// MODEL3D_BATCH_PART_LIT): the body's frame, its SH times the location's
// animT x0.6 (hood) or x0.9 (trunk) -- Render's copied light context
// scaled by [0x8033c1f8] / [0x8033c208].
#define LIT_FRAME0   (PART_FRAME0 + MODEL3D_PART_FRAMES)
#define WHEEL_FRAMES (LIT_FRAME0 + 2)

static Model3DWheels s_nextWheels;
static bool s_nextWheelsSet = false;
static Model3DPartPose s_nextParts;
static bool s_nextPartsSet = false;
static struct {
	const Model3D* m;
	Model3DWheels w;
	Model3DPartPose parts;   // this draw's, all shut without one
	bool wheels;      // the corner frames move
	bool sh;          // carsh is drawing, else car.v.pica
	bool camber;      // the LOD takes the camber
	int bound;        // the frame whose uniforms are up
	u32 built;        // the frames worked out this draw
	float world[10][4];   // carLightWorld's, for the corner frames
	WheelFrame f[WHEEL_FRAMES];
} s_wh;
// A car draw is under way (wheelBegin .. drawCarBatches' end): the opening
// parts' tags mean something.
static bool s_partDraw = false;

void model3d_set_car_wheels(const Model3DWheels* w) {
	s_nextWheelsSet = w != NULL;
	if (w)
		s_nextWheels = *w;
}

void model3d_set_car_parts(const Model3DPartPose* p) {
	s_nextPartsSet = p != NULL;
	if (p)
		s_nextParts = *p;
}

// Render's visibility for a MODEL3D_BATCH_PART_COND batch: its location not
// closed -- or, for the door panels and sills (and the trunk underside of a
// ShowTrunkUnderInFE car, PART_SUB on a trunk batch), any frontend draw.
static bool partShows(u32 flags) {
	const int loc = (int)((flags & MODEL3D_BATCH_CORNER_MASK) >> MODEL3D_BATCH_CORNER_SHIFT);
	if (s_wh.parts.open[loc])
		return true;
	return s_wh.parts.fe && (loc >= 2 || (loc == 1 && (flags & MODEL3D_BATCH_PART_SUB)));
}

// The opening part frame a batch is drawn in, 0 for the body's.
static int partFrame(u32 flags) {
	if (flags & MODEL3D_BATCH_PART_STILL)
		return 0;
	const int loc = (int)((flags & MODEL3D_BATCH_CORNER_MASK) >> MODEL3D_BATCH_CORNER_SHIFT);
	const int f = loc == 0 ? ((flags & MODEL3D_BATCH_PART_SUB) ? 1 : 0) : loc + 1;
	return s_wh.parts.moved[f] ? PART_FRAME0 + f : 0;
}

// carsh's light, eye and reflection axes for a frame whose rot/origin are
// set.
static void wheelShFrame(WheelFrame* f) {
	carLightFrame((const float (*)[4])s_wh.world, (const float (*)[3])f->rot, f->origin,
		f->sh, f->eye);
	// TEXMTX1 (carWorld x hack_man, elSetupEnvMap): the env camera's axes
	// in the frame's model space, so the normal goes straight in.
	for (int k = 0; k < 3; k++)
		for (int j = 0; j < 3; j++)
			f->envm[k][j] = f->rot[j][0] * s_carEnvAxes[k][0] +
				f->rot[j][1] * s_carEnvAxes[k][1] + f->rot[j][2] * s_carEnvAxes[k][2];
}

static void wheelUpload(const WheelFrame* f) {
	if (!s_wh.sh) {
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_carShader.uMvp, (C3D_Mtx*)&f->mvp);
		C3D_FVUnifMtx3x4(GPU_VERTEX_SHADER, s_carShader.uNormalMtx, (C3D_Mtx*)&f->normal);
		return;
	}
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_shShader.uMvp, (C3D_Mtx*)&f->mvp);
	for (int k = 0; k < 10; k++)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uSh + k, f->sh[k][0], f->sh[k][1],
			f->sh[k][2], f->sh[k][3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uEye, f->eye[0], f->eye[1], f->eye[2], 1.0f);
	if (s_carEnvTex && s_shShader.uEnvm >= 0)
		for (int k = 0; k < 3; k++)
			C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uEnvm + k, f->envm[k][0],
				f->envm[k][1], f->envm[k][2], 0.0f);
}

// The body's frame, for a car draw, and its uniforms: `rot`/`origin` as
// model3d_draw_car_xform takes them (carsh only), `normal` car.v.pica's.
// Whether this draw moves its wheels is settled here, and the next draw goes
// back to parked unless told otherwise.
static void wheelBegin(const Model3D* m, const C3D_Mtx* mvp, const C3D_Mtx* normal,
		const float rot[3][3], const float origin[3], bool sh) {
	WheelFrame* f = &s_wh.f[0];
	f->mvp = *mvp;
	if (normal)
		f->normal = *normal;
	if (rot) {
		memcpy(f->rot, rot, sizeof(f->rot));
		memcpy(f->origin, origin, sizeof(f->origin));
	}
	s_wh.m = m;
	s_wh.sh = sh;
	s_wh.bound = 0;
	s_wh.built = 1;
	s_wh.wheels = s_nextWheelsSet && m->wheelPivots;
	s_wh.w = s_nextWheels;
	s_wh.camber = s_nextCarLod <= 1;
	s_nextWheelsSet = false;
	if (s_nextPartsSet)
		s_wh.parts = s_nextParts;
	else
		memset(&s_wh.parts, 0, sizeof(s_wh.parts));
	s_nextPartsSet = false;
	bool partsMove = false;
	for (int f = 0; f < MODEL3D_PART_FRAMES; f++)
		partsMove |= s_wh.parts.moved[f];
	s_whOn = s_wh.wheels || partsMove || (sh && m->carBay);
	s_partDraw = true;
	if (sh) {
		carLightWorld(origin, s_wh.world);
		wheelShFrame(f);
	}
	wheelUpload(f);
}

static void frameCompose(int g, const float R[3][3], const float t[3]);

static void wheelBuild(int g) {
	if (g >= LIT_FRAME0) {
		const int loc = g - LIT_FRAME0;
		WheelFrame* f = &s_wh.f[g];
		*f = s_wh.f[0];
		const float k = s_wh.parts.animT[loc] * (loc ? 0.9f : 0.6f);
		for (int i = 0; i < 10; i++)
			for (int c = 0; c < 4; c++)
				f->sh[i][c] *= k;
		// With a level-3 ENGINE / TRUNK_AUDIO part, that bay's neon lights
		// it (#506): the colour into the constant term, x animT x0.6 on the
		// hood and x1.5 on the trunk (Speed.elf [0x8033c1f8] / [0x8033c1b8];
		// SPEED2.EXE 0x630571 / 0x630793).
		const float kn = s_wh.parts.animT[loc] * (loc ? 1.5f : 0.6f);
		for (int c = 0; c < 3; c++)
			f->sh[0][c] += s_wh.parts.neonAdd[loc][c] * kn;
		s_wh.built |= 1u << g;
		return;
	}
	if (g >= PART_FRAME0) {
		const int p = g - PART_FRAME0;
		frameCompose(g, (const float (*)[3])s_wh.parts.rot[p], s_wh.parts.t[p]);
		return;
	}
	const int c = g & 3, kind = g >> 2;
	const Model3DWheels* w = &s_wh.w;
	const float spin = kind == MODEL3D_WHEEL_RIM ? w->spin[c]
		: kind == MODEL3D_WHEEL_SPINNER ? w->spinner[c] : 0.0f;
	const float steer = c < 2 ? w->steer : 0.0f;
	const float camber = (c < 2 && s_wh.camber) ? (c == 0 ? w->camber : -w->camber) : 0.0f;
	const float sy = sinf(spin), cy = cosf(spin);
	const float sz = sinf(steer), cz = cosf(steer);
	const float sx = sinf(camber), cx = cosf(camber);
	// Rz Ry, then Rx over it: column vectors, model space (x front, y left,
	// z up); each one right-handed, as eRotateX/Y/Z are.
	const float zy[3][3] = {
		{ cz * cy, -sz, cz * sy },
		{ sz * cy,  cz, sz * sy },
		{ -sy,      0.0f, cy },
	};
	float R[3][3];
	for (int j = 0; j < 3; j++) {
		R[0][j] = zy[0][j];
		R[1][j] = cx * zy[1][j] - sx * zy[2][j];
		R[2][j] = sx * zy[1][j] + cx * zy[2][j];
	}
	const float* p = s_wh.m->wheelPivot[c];
	float t[3];
	for (int i = 0; i < 3; i++)
		t[i] = p[i] - (R[i][0] * p[0] + R[i][1] * p[1] + R[i][2] * p[2]);
	t[2] += w->susp[c];
	frameCompose(g, (const float (*)[3])R, t);
}

// Frame g = the body's frame times D, D = [R t] in model space (column
// vectors): its mvp, normal matrix or carsh frame.
static void frameCompose(int g, const float R[3][3], const float t[3]) {
	const WheelFrame* b = &s_wh.f[0];
	WheelFrame* f = &s_wh.f[g];
	C3D_Mtx D;
	Mtx_Identity(&D);
	for (int i = 0; i < 3; i++) {
		D.r[i].x = R[i][0];
		D.r[i].y = R[i][1];
		D.r[i].z = R[i][2];
	}
	if (!s_wh.sh) {
		Mtx_Multiply(&f->normal, (C3D_Mtx*)&b->normal, &D);
	}
	for (int i = 0; i < 3; i++)
		D.r[i].w = t[i];
	Mtx_Multiply(&f->mvp, (C3D_Mtx*)&b->mvp, &D);
	if (s_wh.sh) {
		// rot'[j] = sum_a R[a][j] rot[a], origin' = origin + sum_a t[a] rot[a].
		for (int j = 0; j < 3; j++)
			for (int k = 0; k < 3; k++)
				f->rot[j][k] = R[0][j] * b->rot[0][k] + R[1][j] * b->rot[1][k] +
					R[2][j] * b->rot[2][k];
		for (int k = 0; k < 3; k++)
			f->origin[k] = b->origin[k] + t[0] * b->rot[0][k] + t[1] * b->rot[1][k] +
				t[2] * b->rot[2][k];
		wheelShFrame(f);
	}
	s_wh.built |= 1u << g;
}

static void wheelBind(const ModelBatch* b) {
	const u32 bits = b->flags & (MODEL3D_BATCH_CORNER_MASK | MODEL3D_BATCH_WHEEL_MASK);
	const int g = (b->flags & MODEL3D_BATCH_PART)
		? ((b->flags & MODEL3D_BATCH_PART_LIT) && s_wh.sh
		   ? LIT_FRAME0 + (((b->flags & MODEL3D_BATCH_CORNER_MASK) >>
		                    MODEL3D_BATCH_CORNER_SHIFT) ? 1 : 0)
		   : partFrame(b->flags))
		: (s_wh.wheels && (b->flags & MODEL3D_BATCH_WHEEL_MASK))
		? (int)(bits >> MODEL3D_BATCH_CORNER_SHIFT) : 0;
	if (g == s_wh.bound)
		return;
	if (!(s_wh.built & (1u << g)))
		wheelBuild(g);
	wheelUpload(&s_wh.f[g]);
	s_wh.bound = g;
}

// The SH car is being drawn (drawCarSh), and the material its uniforms carry
// (-1 for none yet).
static bool s_shDraw = false;
static int  s_shBoundMat = -1;

// UpdatePlatInfo's values for the batch, into the uniforms carsh.v.pica reads.
static void shSetMaterial(u8 index) {
	if ((int)index == s_shBoundMat)
		return;
	const LightMat* L = lightmat_at(index);
	if (!L)
		L = lightmat_get(LIGHTMAT_DEFAULT);
	if (!L)
		return;
	s_shBoundMat = index;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uMatD0,
		L->diffMin[0], L->diffMin[1], L->diffMin[2], L->minA);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uMatD1,
		L->diffRange[0], L->diffRange[1], L->diffRange[2], L->rangeA);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uMatS0,
		L->specMin[0], L->specMin[1], L->specMin[2], 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uMatS1,
		L->specRange[0], L->specRange[1], L->specRange[2], 0.0f);
	// PORT (#312): SPEED2.EXE's car material setter uploads SpecularPower as
	// the material's times 1.1 (0x5bda53) and EnvmapPower as it is.
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uMatP,
		L->specPower * 1.1f, L->envPower, 0.0f, 0.0f);
	if (s_shShader.uEnvm >= 0) {
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uEnvK0,
			L->k0[0] / 255.0f, L->k0[1] / 255.0f, L->k0[2] / 255.0f, 0.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_shShader.uEnvK1,
			L->k1[0] / 255.0f, L->k1[1] / 255.0f, L->k1[2] / 255.0f, 0.0f);
	}
}

// Whether a light material's gloss (env: its reflection) can add anything at
// all (#352). Both passes add a colour times CLR0.a = sat(MinA + RangeA d),
// d in [0.01, 1]; the gloss's colour is SH(R) (SpecMin + SpecRange d^Ps) I and
// the reflection's lerp(K0, K1, d^Pe), so with that alpha at 0 everywhere, or
// SpecMin = SpecRange = 0 (K0 = K1 = 0), the pass adds exactly 0 and its draw
// is left out. Worked out once per material; the table does not change.
int g_dbgNoPassSkip = 0;
// #350: off. On the 3DS the rebind-only path drew some batches with the wrong
// texture -- a shop front's glass came out as other buildings' facades, solid,
// over the storefront -- although the command stream it leaves is the same
// stage words the full rebuild writes. Found live over Luma's GDB at a paused
// frame that showed it: g_dbgNoTevKeep=1 drew the storefront right 4 times of
// 4, 0 wrong 4 times of 4. Azahar draws both the same. Mode 2 keeps the CPU
// saving but re-emits stage 0; on the 3DS it was still wrong 6 times of 6
// (mode 1 wrong 4 of 5, mode 0 right 3 of 3, same paused frame), so the TEV
// write alone is not what the hardware needs. The one other register the full
// rebuild always writes is the alpha test.
int g_tevKeep = 0;
// citro3d's own: marks every float uniform of one shader type for upload.
// Not in its public headers; DbgForce's bit 8 calls it.
void C3Di_DirtyUniforms(GPU_SHADER_TYPE type);
int g_dbgForce = 0;
// What of g_dbgForce the model being drawn takes: world-pass models with
// materials only, outside the front end -- the bit 4 reset writes the world's
// opaque state, which the sky, decals, logos and loading screens do not use.
static int s_forceBits = 0;
static u8 s_matLive[256];   // bit 0 worked out, 1 gloss adds, 2 env adds
static bool matAdds(u8 index, bool env) {
	if (g_dbgNoPassSkip)
		return true;
	if (!(s_matLive[index] & 1u)) {
		const LightMat* L = lightmat_at(index);
		if (!L)
			L = lightmat_get(LIGHTMAT_DEFAULT);
		u8 v = 1u | 2u | 4u;
		if (L) {
			const float a0 = L->minA + 0.01f * L->rangeA, a1 = L->minA + L->rangeA;
			const bool alpha = a0 > 0.0f || a1 > 0.0f;
			bool spec = false, refl = false;
			for (int c = 0; c < 3; c++) {
				spec = spec || L->specMin[c] != 0.0f || L->specRange[c] != 0.0f;
				refl = refl || L->k0[c] || L->k1[c];
			}
			v = (u8)(1u | (alpha && spec ? 2u : 0u) | (alpha && refl ? 4u : 0u));
		}
		s_matLive[index] = v;
	}
	return (s_matLive[index] & (env ? 4u : 2u)) != 0;
}

void model3d_dead_passes(const Model3D* m, int* gloss, int* env) {
	*gloss = *env = 0;
	if (!m || !m->lightMat)
		return;
	for (int i = 0; i < m->numBatches; i++) {
		*gloss += !matAdds(m->lightMat[i], false);
		*env += (m->batches[i].flags & 0x100u) && !matAdds(m->lightMat[i], true);
	}
}

// A batch the gloss pass goes over, as the first pass drew it.
typedef struct {
	const ModelBatch* b;
	const C3D_Tex* bind;  // NULL: untextured
	u8 mat;
	u8 cut;
	u8 need;
	u8 cull;
} GlossRec;
#define GLOSS_MAX 1024
static GlossRec s_gloss[GLOSS_MAX];

// ps_Model mode 7's specular half: 4 tex.a CLR0.a CLR1 added over what the
// first pass drew, the alpha that pass made (2 tex.a CLR0.a) beside it for
// the cutout test and for a blended batch's coverage. Additive with no depth
// write, at GEQUAL so it lands on the first pass's own depth, and fogged to
// black so the fog the first pass put in is not added twice.
static void drawGloss(const Model3D* m, const GlossRec* r, int n, bool blended) {
	C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_shShader.uGloss, true);
	if (s_fogOn)
		C3D_FogColor(0);
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, blended ? GPU_SRC_ALPHA : GPU_ONE, GPU_ONE,
		GPU_ZERO, GPU_ONE);
	const C3D_Tex* bound = NULL;
	int boundCut = -1;
	bool first = true;
	for (int i = 0; i < n; i++, r++) {
		if (!matAdds(r->mat, false))
			continue;
		shSetMaterial(r->mat);
		if (first || r->bind != bound) {
			first = false;
			C3D_TexEnv* env = C3D_GetTexEnv(0);
			C3D_TexEnvInit(env);
			if (r->bind) {
				C3D_TexBind(0, (C3D_Tex*)r->bind);
				C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR,
					GPU_PRIMARY_COLOR);
			} else {
				// No texture: the first pass's paint constant is opaque, so
				// its "tex.a" is 1.
				C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_PRIMARY_COLOR,
					GPU_PRIMARY_COLOR);
				C3D_TexEnvColor(env, 0xFFFFFFFFu);
			}
			C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR,
				GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
			C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_4);
			C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
			C3D_SetTexEnv(0, env);
			bound = r->bind;
		}
		if (r->cut != boundCut) {
			C3D_AlphaTest(r->cut != 0, GPU_GREATER, 0x40);
			boundCut = r->cut;
		}
		drawBatchGeometry(m, r->b, r->need, r->cull);
	}
	C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_shShader.uGloss, false);
	if (s_fogOn)
		C3D_FogColor(s_fogColor);
}

// The strip flag that turns a car batch's reflection on: the GameCube's
// solid draw takes K0/K1 and the sphere map only where it is set (#166).
// Kit batches keep it in their low bits (CARSKIN, CHROME, the glasses...;
// BOTTOM and INTERIOR go without).
#define CAR_BATCH_ENV 0x100u

// Turns a texture unit off again, which C3D_TexBind(unit, NULL) cannot on 1 or 2:
// citro3d 1.7.1's TexBind reads tex->param first, to refuse a cube map on
// those units, so the NULL faults there -- a data abort at 0xC on the 3DS,
// where Azahar reads 0 and carries on. This stores what TexBind stores past
// that check: NULL in the unit's slot of __C3D_Context's tex[] (+0x118) and
// the unit's dirty flag, BIT(23 + unit) of its flags (+0x20), the layout
// libcitro3d's own TexBind uses. Only when the slot holds `bound`, the
// texture bound there last, so a citro3d laid out otherwise leaves the unit
// on (a fetch no TEV stage reads) rather than writing over something else.
extern u8 __C3D_Context[];
void model3d_tex_unit_off(int unit, const C3D_Tex* bound) {
	const C3D_Tex** slot = (const C3D_Tex**)(void*)(__C3D_Context + 0x118) + unit;
	if (*slot != bound)
		return;
	*slot = NULL;
	*(u32*)(void*)(__C3D_Context + 0x20) |= 1u << (23 + unit);
}

// The first traced world frame draws the reflection with unit 1 off (#308):
// carenv's vertex work alone, the sphere map's texel a constant grey. If that
// frame's reflection stage hangs on the 3DS, the program is at fault; if the
// next one's does, unit 1.
static bool s_envNoSphere = false;

// ps_Model mode 7's reflection, S2-S5: 4 tex.a CLR0.a lerp(K1, K0, CLR1.a)
// (since #312 the PC's 8 tex.a CLR0.a lerp(K0, K1, d^Pe), carenv.v.pica)
// times the sphere map's texel at the env-space normal, added over the
// batches with CAR_BATCH_ENV, as drawGloss adds the specular. carenv.v.pica
// does the vertex half (#309: the 3DS hung on it as a branch of carsh);
// stage 0 is tex.a times its colour (the alpha as the gloss pass has it),
// stage 1 takes the sphere map from unit 1 at x4.
static void drawEnv(const Model3D* m, const GlossRec* r, int n, bool blended) {
	if (!s_carEnvTex || s_shShader.uEnvm < 0)
		return;
	int any = 0;
	for (int i = 0; i < n && !any; i++)
		any = (r[i].b->flags & CAR_BATCH_ENV) && matAdds(r[i].mat, true);
	if (!any)
		return;
	s_envNoSphere = gpuwatch_trace_frame() == 1;
	bindProg(&s_shShader.envProgram);
	if (s_fogOn)
		C3D_FogColor(0);
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, blended ? GPU_SRC_ALPHA : GPU_ONE, GPU_ONE,
		GPU_ZERO, GPU_ONE);
	// citro2d leaves its procedural texture (unit 3) on, reading texcoord1
	// (C3D_ProcTexBind(1, ...) for its circles and blends; C2D_Prepare() puts
	// it back), the coordinate unit 1 samples here. No TEV stage of a 3D pass
	// reads unit 3, so it goes off rather than run beside unit 1.
	C3D_ProcTexBind(0, NULL);
	if (!s_envNoSphere)
		C3D_TexBind(1, (C3D_Tex*)s_carEnvTex);
	C3D_TexEnv* e1 = C3D_GetTexEnv(1);
	C3D_TexEnvInit(e1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, s_envNoSphere ? GPU_CONSTANT : GPU_TEXTURE1,
		GPU_PREVIOUS);
	C3D_TexEnvColor(e1, 0xFF808080u);
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvScale(e1, C3D_RGB, GPU_TEVSCALE_4);
	C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
	C3D_SetTexEnv(1, e1);
	const C3D_Tex* bound = NULL;
	int boundCut = -1;
	bool first = true;
	for (int i = 0; i < n; i++, r++) {
		if (!(r->b->flags & CAR_BATCH_ENV) || !matAdds(r->mat, true))
			continue;
		shSetMaterial(r->mat);
		if (first || r->bind != bound) {
			C3D_TexEnv* env = C3D_GetTexEnv(0);
			C3D_TexEnvInit(env);
			if (r->bind) {
				C3D_TexBind(0, (C3D_Tex*)r->bind);
				C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR,
					GPU_PRIMARY_COLOR);
			} else {
				C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_PRIMARY_COLOR,
					GPU_PRIMARY_COLOR);
				C3D_TexEnvColor(env, 0xFFFFFFFFu);
			}
			C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR,
				GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
			C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
			C3D_SetTexEnv(0, env);
			bound = r->bind;
			first = false;
		}
		if (r->cut != boundCut) {
			C3D_AlphaTest(r->cut != 0, GPU_GREATER, 0x40);
			boundCut = r->cut;
		}
		drawBatchGeometry(m, r->b, r->need, r->cull);
	}
	model3d_tex_unit_off(1, s_carEnvTex);
	e1 = C3D_GetTexEnv(1);
	C3D_TexEnvInit(e1);
	C3D_SetTexEnv(1, e1);
	bindProg(&s_shShader.program);
	if (s_fogOn)
		C3D_FogColor(s_fogColor);
}

// The reflection lerp's weights back from texcoord2 (#431): a 256x256 RGBA8
// ramp, nearest, clamped, whose texel at (s, t) holds s in R, G and B and t
// in A, so unit 2 hands the combiner carge.v.pica's two weights at 8 bits --
// the vertex colour's own precision. Rows run from the image's top (t = 1),
// as every texture here is tiled.
int g_dbgNoGlossEnv = 0;
bool g_sweepGlossEnvSplit = false;
static u8 mortonIndex(u32 x, u32 y);
static C3D_Tex s_geRamp;
static int s_geRampState;   // 0 not built yet, 1 built, -1 failed
static bool geRamp(void) {
	if (s_geRampState)
		return s_geRampState > 0;
	s_geRampState = -1;
	if (!C3D_TexInit(&s_geRamp, 256, 256, GPU_RGBA8))
		return false;
	u32* dst = (u32*)s_geRamp.data;
	for (u32 y = 0; y < 256; y++) {
		const u32 a = 255u - y;
		const u32 rowAt = (y >> 3) * 32u * 64u;
		for (u32 x = 0; x < 256; x++)
			dst[rowAt + (x >> 3) * 64u + mortonIndex(x & 7, y & 7)] =
				(x << 24) | (x << 16) | (x << 8) | a;
	}
	C3D_TexFlush(&s_geRamp);
	C3D_TexSetFilter(&s_geRamp, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&s_geRamp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	s_geRampState = 1;
	dbglog_line(NULL, "carge: gloss + reflection in one pass over the car's opaque batches (ramp 256x256, 256 KB)");
	return true;
}

static u32 tevK(const u8 k[3]) {
	return 0xFF000000u | ((u32)k[2] << 16) | ((u32)k[1] << 8) | k[0];
}

// drawGloss() and drawEnv() over the opaque batches in one draw each (#431),
// carge.v.pica's vertex half. The two passes add with the same blend and
// depth, and sat(sat(f + g) + e) = sat(f + sat(g + e)) for g, e >= 0, so one
// draw adding g + e leaves the same pixel:
//
//   S0  4 tex.a CLR0.rgb, alpha 2 tex.a CLR0.a   drawGloss's stage, kept in
//                                                the combiner buffer
//   S1  w0 K0                                    the weights from unit 2
//   S2  sat(2 (w1 K1 + prev))                    carenv's colour
//   S3  prev tex.a                               drawEnv's stage 0
//   S4  prev sphere x4                           drawEnv's stage 1
//   S5  prev + buffer
//
// A batch without the reflection (no CAR_BATCH_ENV, or K0 = K1 = 0) takes
// K0 = K1 = 0, so S1-S4 add nothing; one whose gloss adds nothing has
// SpecMin = SpecRange = 0, so S0 is 0. The blended batches keep their two
// draws: SRC_ALPHA scales what they add, and the clamp would differ.
// False when the program, the sphere or the ramp is missing (the caller draws
// the two passes).
static bool drawGlossEnv(const Model3D* m, const GlossRec* r, int n) {
	if (g_dbgNoGlossEnv || g_sweepGlossEnvSplit || !s_shShader.geReady || !s_carEnvTex || s_shShader.uEnvm < 0 ||
			!geRamp())
		return false;
	s_envNoSphere = gpuwatch_trace_frame() == 1;
	bindProg(&s_shShader.geProgram);
	if (s_fogOn)
		C3D_FogColor(0);
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
	// As drawEnv(): citro2d's procedural texture off before unit 1 goes on.
	C3D_ProcTexBind(0, NULL);
	if (!s_envNoSphere)
		C3D_TexBind(1, (C3D_Tex*)s_carEnvTex);
	C3D_TexBind(2, &s_geRamp);
	C3D_TexEnvBufUpdate(C3D_RGB, 1);

	C3D_TexEnv* e = C3D_GetTexEnv(1);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE2, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(e, 0);

	e = C3D_GetTexEnv(2);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE2, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR,
		GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MULTIPLY_ADD);
	C3D_TexEnvScale(e, C3D_RGB, GPU_TEVSCALE_2);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(e, 0);

	e = C3D_GetTexEnv(4);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, s_envNoSphere ? GPU_CONSTANT : GPU_TEXTURE1,
		GPU_PREVIOUS);
	C3D_TexEnvColor(e, 0xFF808080u);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvScale(e, C3D_RGB, GPU_TEVSCALE_4);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);

	e = C3D_GetTexEnv(5);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS_BUFFER, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_ADD);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);

	const C3D_Tex* bound = NULL;
	int boundCut = -1;
	u32 boundK0 = 0, boundK1 = 0;
	bool first = true;
	for (int i = 0; i < n; i++, r++) {
		const bool env = (r->b->flags & CAR_BATCH_ENV) && matAdds(r->mat, true);
		if (!env && !matAdds(r->mat, false))
			continue;
		shSetMaterial(r->mat);
		u32 k0 = 0, k1 = 0;
		if (env) {
			const LightMat* L = lightmat_at(r->mat);
			if (!L)
				L = lightmat_get(LIGHTMAT_DEFAULT);
			if (L) {
				k0 = tevK(L->k0);
				k1 = tevK(L->k1);
			}
		}
		if (k0 != boundK0) {
			C3D_TexEnvColor(C3D_GetTexEnv(1), k0);
			boundK0 = k0;
		}
		if (k1 != boundK1) {
			C3D_TexEnvColor(C3D_GetTexEnv(2), k1);
			boundK1 = k1;
		}
		if (first || r->bind != bound) {
			first = false;
			C3D_TexEnv* s0 = C3D_GetTexEnv(0);
			C3D_TexEnvInit(s0);
			C3D_TexEnv* s3 = C3D_GetTexEnv(3);
			C3D_TexEnvInit(s3);
			if (r->bind) {
				C3D_TexBind(0, (C3D_Tex*)r->bind);
				C3D_TexEnvSrc(s0, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR,
					GPU_PRIMARY_COLOR);
				C3D_TexEnvSrc(s3, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, GPU_PREVIOUS);
			} else {
				// No texture: the first pass's paint constant is opaque, so its
				// "tex.a" is 1 (drawGloss).
				C3D_TexEnvSrc(s0, C3D_Both, GPU_CONSTANT, GPU_PRIMARY_COLOR,
					GPU_PRIMARY_COLOR);
				C3D_TexEnvColor(s0, 0xFFFFFFFFu);
				C3D_TexEnvSrc(s3, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
				C3D_TexEnvColor(s3, 0xFFFFFFFFu);
			}
			C3D_TexEnvOpRgb(s0, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR,
				GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvFunc(s0, C3D_Both, GPU_MODULATE);
			C3D_TexEnvScale(s0, C3D_RGB, GPU_TEVSCALE_4);
			C3D_TexEnvScale(s0, C3D_Alpha, GPU_TEVSCALE_2);
			C3D_TexEnvOpRgb(s3, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA,
				GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvFunc(s3, C3D_RGB, GPU_MODULATE);
			C3D_TexEnvSrc(s3, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
			C3D_TexEnvFunc(s3, C3D_Alpha, GPU_REPLACE);
			bound = r->bind;
		}
		if (r->cut != boundCut) {
			C3D_AlphaTest(r->cut != 0, GPU_GREATER, 0x40);
			boundCut = r->cut;
		}
		drawBatchGeometry(m, r->b, r->need, r->cull);
	}

	C3D_TexEnvBufUpdate(C3D_Both, 0);
	model3d_tex_unit_off(1, s_carEnvTex);
	model3d_tex_unit_off(2, &s_geRamp);
	for (int st = 1; st <= 5; st++) {
		e = C3D_GetTexEnv(st);
		C3D_TexEnvInit(e);
		C3D_SetTexEnv(st, e);
	}
	bindProg(&s_shShader.program);
	if (s_fogOn)
		C3D_FogColor(s_fogColor);
	return true;
}

// TEV stages 1 and 2 back to pass-through, when a tint left them live.
static void resetPaintStages(bool* boundPaint) {
	if (!*boundPaint)
		return;
	C3D_TexEnv* e = C3D_GetTexEnv(1);
	C3D_TexEnvInit(e);
	C3D_SetTexEnv(1, e);
	e = C3D_GetTexEnv(2);
	C3D_TexEnvInit(e);
	C3D_SetTexEnv(2, e);
	*boundPaint = false;
}

// The projected headlight's half of drawBatchPass (#296, model3d.h).
//
// Whether a batch's box meets the beam's textured frustum: the corner
// furthest along each plane's normal, outside when that is behind one.
static bool hlBoxIn(const float lo[3], const float hi[3])
{
	for (int k = 0; k < 6; k++) {
		const float* p = s_hl.planes[k];
		const float d = p[0] * (p[0] >= 0.0f ? hi[0] : lo[0]) +
		                p[1] * (p[1] >= 0.0f ? hi[1] : lo[1]) +
		                p[2] * (p[2] >= 0.0f ? hi[2] : lo[2]) + p[3];
		if (d < 0.0f)
			return false;
	}
	return true;
}

// Into or out of scenehl.v.pica. Its uniforms after scene.v.pica's are
// written on every switch in: another program's may have used those
// registers since. Out puts back what the plain path expects -- the scene
// program, stages 1 to 3 passing through and units 0 to 2 off; the next
// textured batch rebinds unit 0. Off, not left on: the beam is a projection
// texture, and scene.v.pica writes no texcoord0w for it to divide by (#327).
static void hlSwitch(bool on, const C3D_Tex* unit1)
{
	s_hlSwitches++;
	if (on) {
		bindProg(&s_hlShader.program);
		// The projector's rows take the world position too, so the origin
		// folds into them as into mvp.
		for (int k = 0; k < 4; k++) {
			const float* r = s_hl.rows[k];
			const float w = s_originOn
				? r[3] + r[0] * s_origin[0] + r[1] * s_origin[1] + r[2] * s_origin[2] : r[3];
			C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uHl + k, r[0], r[1], r[2], w);
		}
		return;
	}
	bindProg(&s_sceneShader.program);
	for (int st = 1; st <= 3; st++) {
		C3D_TexEnv* e = C3D_GetTexEnv(st);
		C3D_TexEnvInit(e);
		C3D_SetTexEnv(st, e);
	}
	model3d_tex_unit_off(0, s_hl.beam);
	if (unit1)
		model3d_tex_unit_off(1, unit1);
	model3d_tex_unit_off(2, s_hl.clip);
}

// worldheadlight.efx's pixel shader, rgb = sat(2 t0 (t1 t2 K + v0)), on the
// port's world colour c = 2 v0 (scene.v.pica's, which the plain path draws
// as t0 c) and K = HeadlightColor:
//
//   S0  t1 * t2                     the beam's texel times the clip
//   S1  prev * K/2                  the bulb constant, colour / 255
//   S2  prev * t0, x4               2 t0 (t1 t2 K), clamped
//   S3  t0 * c + prev               plus the plain pixel, clamped
//
// Every term is positive, so clamping S2 before S3 adds changes nothing,
// and where the beam is black S3 is t0 c, the plain path's pixel to the bit.
// The alpha is the plain path's: t0.a times the colour's 1.
//
// The procedural texture goes off first: citro2d leaves unit 3 on, reading
// texcoord1 (drawEnv() says more), and this is the first world program that
// writes texcoord1 and samples it on unit 1. (#327's hang was scenehl.v.pica's
// unwritten output components; see that file.)
//
// `overlay` (#395, hlOverlay) leaves S3 passing S2 through: the beam's term
// alone, for adding over the plain pixel the batch's own draw already wrote.
// #173: a blended batch that writes no depth draws with the alpha test on at
// GREATER 0. Every blend the world and the cars use weights the fragment by
// its own alpha (SRC_ALPHA with ONE or ONE_MINUS_SRC_ALPHA), so a fragment of
// alpha 0 adds nothing and leaves the framebuffer as it was -- dropping it is
// pixel-exact. What it saves is the PICA's framebuffer read and write for it:
// the faded rims of the glow sprites (SFX_FLARE_GLOWA), the cut-out fringes of
// the foliage and the decals. A batch that writes depth keeps it off, since its
// alpha-0 texels still write depth. DbgBlendCut=0 turns it off.
u32 g_blendCut = 1;

static inline void cutSet(int state)
{
	if (state == 1)
		C3D_AlphaTest(true, GPU_GREATER, 0x40);
	else if (state == 2)
		C3D_AlphaTest(true, GPU_GREATER, 0x00);
	else
		C3D_AlphaTest(false, GPU_ALWAYS, 0x00);
}

static void hlStages(C3D_TexEnv* env, const C3D_Tex* diffuse, bool overlay)
{
	C3D_ProcTexBind(0, NULL);
	C3D_TexBind(0, s_hl.beam);
	C3D_TexBind(1, (C3D_Tex*)diffuse);
	C3D_TexBind(2, s_hl.clip);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE2, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	C3D_SetTexEnv(0, env);

	C3D_TexEnv* e = C3D_GetTexEnv(1);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvColor(e, s_hl.colour);
	C3D_SetTexEnv(1, e);

	e = C3D_GetTexEnv(2);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvScale(e, C3D_RGB, GPU_TEVSCALE_4);
	C3D_SetTexEnv(2, e);

	e = C3D_GetTexEnv(3);
	C3D_TexEnvInit(e);
	if (!overlay) {
		C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_MULTIPLY_ADD);
	}
	C3D_SetTexEnv(3, e);
}

// ------------------------------------------- the road's reflection (#296)
//
// The world batch programs: scene.v.pica, scenehl.v.pica (the beam) and
// scenerf.v.pica (the capture).
enum { PROG_SCENE, PROG_HL, PROG_RF };

// Out of scenerf.v.pica: what hlSwitch(false) leaves -- the scene program,
// stages 1 to 3 passing through and units 0 and 1 off (the capture is a
// projection texture too) -- and the combiner buffer back off.
static void rfOff(const C3D_Tex* unit1)
{
	bindProg(&s_sceneShader.program);
	for (int st = 1; st <= 3; st++) {
		C3D_TexEnv* e = C3D_GetTexEnv(st);
		C3D_TexEnvInit(e);
		C3D_SetTexEnv(st, e);
	}
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	model3d_tex_unit_off(0, s_rfTex);
	if (unit1)
		model3d_tex_unit_off(1, unit1);
}

// From one world batch program to another. Leaving the beam or the capture
// puts the plain path's state back first; entering the beam uploads its rows.
static void progSwitch(int from, int to, const C3D_Tex* unit1)
{
	if (from == to)
		return;
	if (from == PROG_HL)
		hlSwitch(false, unit1);
	else if (from == PROG_RF)
		rfOff(unit1);
	if (to == PROG_HL)
		hlSwitch(true, NULL);
	else if (to == PROG_RF)
		bindProg(&s_rfShader.program);
}

// ps_Model(2) on the port's plain pixel P = t1 * c (the diffuse on unit 1
// now, the capture taking unit 0's projection), with a = t1.a:
//
//   S0  t1 * c, t1.a                 P and a; into the combiner buffer
//   S1  t0 * 1/2, a * 1/4            the capture halved, a / 4
//   S2  prev * buf.a, prev.a         capture * a / 2
//   S3  buf * (1 - prev.a) + prev    P (1 - a / 4) + capture * a / 2
//
// The GameCube's S0 alpha is tex.a K0.a / 2 with K0.a 255 * Rain's +0x43e8,
// the road's dampness (#315: SetRainBase sets it to 1, Rain::UpdateAndRender
// dries it toward 0 while no rain falls), and its S1 (2 - A) C + A T on
// C = tex * ras, P / 2: the same sum. The dampness scales a here: both of S1's
// constants (s_wetK), so P (1 - a d / 4) + capture * a d / 2. The alpha
// out is the plain path's, t1.a. (The PC's adds t1.a * vertex.a * capture to
// P instead; its vertex alpha is ~0.5, the GameCube's / 2, but the port keeps
// no vertex alpha.)
static u32 s_wetK = 0x40808080u;   // 1/2 RGB, 1/4 alpha, times the dampness

void model3d_set_wet_damp(float d)
{
	if (d < 0.0f) d = 0.0f;
	if (d > 1.0f) d = 1.0f;
	const u32 rgb = (u32)(0x80 * d + 0.5f), a = (u32)(0x40 * d + 0.5f);
	s_wetK = (a << 24) | (rgb << 16) | (rgb << 8) | rgb;
}

static void rfStages(C3D_TexEnv* env, const C3D_Tex* diffuse)
{
	C3D_ProcTexBind(0, NULL);   // unit 3, as hlStages()
	C3D_TexBind(0, (C3D_Tex*)s_rfTex);
	C3D_TexBind(1, (C3D_Tex*)diffuse);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_SetTexEnv(0, env);
	C3D_TexEnvBufUpdate(C3D_Both, 1);

	C3D_TexEnv* e = C3D_GetTexEnv(1);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvColor(e, s_wetK);
	C3D_SetTexEnv(1, e);

	e = C3D_GetTexEnv(2);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS_BUFFER, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA,
		GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
	C3D_SetTexEnv(2, e);

	e = C3D_GetTexEnv(3);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS_BUFFER, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA,
		GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MULTIPLY_ADD);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS_BUFFER, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
	C3D_SetTexEnv(3, e);
}

// A reflective batch in the beam as well: retail's ps_Model(0x10) is both at
// once, but the beam and the capture both want unit 0's projection. So the
// beam's pass goes down first and this one over it, blended to
// dst (1 - a / 4) + capture * a / 2 -- the same sum on the beam's pixel. Same
// program position math, so GPU_GEQUAL finds the depths it wrote. Fogged to
// black, as drawGloss and drawEnv are, so the fog the beam's pass put in is
// not added twice. #328: it used to turn the fog off and on around itself,
// which the 3DS paints pale blue (model3d_fog_enable).
// Leaves the capture program bound and the opaque pass's blend and depth up.
// #328's probe (model3d_set_wet_probe): 0 as above, 1 no overlay at all,
// 2 the fog off and on again as the 17:08 build had it (the blue, for a
// hardware check of the fix), 3 a framebuffer flush between the beam's draw
// and this one's state.
static int s_wetProbe;

void model3d_set_wet_probe(int mode)
{
	s_wetProbe = mode;
}

static void rfOverlay(const Model3D* m, const ModelBatch* b, u8 need, int cullState,
		const C3D_Tex* diffuse)
{
	if (s_wetProbe == 3)
		GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
	const bool toggle = s_fogOn && s_wetProbe == 2;
	bindProg(&s_rfShader.program);
	C3D_ProcTexBind(0, NULL);   // unit 3, as hlStages()
	C3D_TexBind(0, (C3D_Tex*)s_rfTex);
	C3D_TexBind(1, (C3D_Tex*)diffuse);
	C3D_TexEnv* e = C3D_GetTexEnv(0);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA,
		GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_TEXTURE1, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvColor(e, s_wetK);
	C3D_SetTexEnv(0, e);
	e = C3D_GetTexEnv(1);
	C3D_TexEnvInit(e);
	C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
	C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(e, s_wetK);
	C3D_SetTexEnv(1, e);
	for (int st = 2; st <= 3; st++) {
		e = C3D_GetTexEnv(st);
		C3D_TexEnvInit(e);
		C3D_SetTexEnv(st, e);
	}
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	model3d_tex_unit_off(2, s_hl.clip);
	if (toggle) {
		// The 17:08 build's way, unflushed and with the LUT going up again.
		C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
		C3D_FogLutBind(NULL);
	} else if (s_fogOn) {
		C3D_FogColor(0);
	}
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ZERO, GPU_ONE);
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
	drawBatchGeometry(m, b, need, cullState);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	if (toggle) {
		C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
		C3D_FogLutBind(&s_fogLut);
	} else if (s_fogOn) {
		C3D_FogColor(s_fogColor);
	}
}

// The beam over the batches its frustum meets (#395). A tester's 3DS lost
// the static world -- roofs, tunnel ceilings, sidewalks -- frame to frame
// with the Headlights option on, and only then; Azahar never showed it. The
// beam used to draw each such batch through scenehl.v.pica in place of its
// plain draw, switching programs batch by batch, so whatever the hardware
// does with that path decided whether the batch appeared at all.
//
// hlStages()' sum is the plain pixel plus a term: S3 = t1 c + sat(2 t0 t1 t2
// K). So each batch now goes down through the plain program (or the wet
// road's) exactly as with the beam off, and this lays the term over it once
// the section's opaque batches are all down: one program switch a section,
// additive, at GEQUAL with no depth write so it lands on the batch's own
// depth (the programs' position math is the same, as rfOverlay() relies
// on), fogged to black. The fog unit mixes the term by the same f the plain
// draw's colour got, so f (c + b) + (1 - f) fog is what both ways give.
// The clamp differs only above white. A blended batch gets its term at once
// after its own draw, scaled by its alpha as the inline sum was.
// PORT: a reflective batch's term now lands after the capture's
// dst (1 - a / 4), not before it -- the beam brighter by at most a quarter
// on the wet road's most reflective texels.
//
// Leaves scenehl.v.pica bound with its stages up; returns the diffuse it
// left on unit 1 for progSwitch(). The caller puts the blend, depth, fog
// colour and alpha test back.
typedef struct {
	const Model3D* m;       // whose buffers the batch is in
	const ModelBatch* b;
	const C3D_Tex* bind;
	float us, ut;           // its texture scroll (#269), the uvoff it drew with
	u8 cut;
	u8 need;
	u8 cull;
	u8 full;                // #412 one pass: the whole pixel, its plain draw skipped
} HlRec;
#define HL_MAX 512
static HlRec s_hlRec[HL_MAX];

// #412: the world's beam, queued across every section and laid over them all
// at once when the opaque phase is done (model3d_beam_flush). The user's look
// sweep at J29.1 put the beam at 8.6 ms of GPU for 180 batches and only 3112
// triangles: the cost was per section -- two program switches each (into
// scenehl and back), with their uniforms and #416's prime draw -- not the
// pixels. One switch pair a frame now. Order cannot show: the term is added
// (saturating adds commute), at GEQUAL with no depth write, fogged to black,
// so a batch a later section then hides fails the depth test where before it
// was painted over -- the same pixel either way. g_hlDefer 0 (config
// DbgBeamDefer=0) lays it per section again, for an A/B.
#define HLQ_MAX 1024
static HlRec s_hlQ[HLQ_MAX];
static int s_hlQn = 0;
static bool s_hlDeferOn = false;
u32 g_hlDefer = 1;

// #412 one pass. The hardware tour (2026-11-08) still had the beam at 4 to 12
// ms of GPU after the cull, the deferral and the scissor: its pixels. Every
// opaque piece in the beam was rasterised twice -- plain, then the additive
// term at GEQUAL with three texture reads and a blend. hlStages()' full sum
// (S3 = t1 c + term) is the plain pixel and the beam in one, the way the beam
// was drawn before #395. So an opaque, dry, untinted batch in the beam now
// has its plain draw skip the pieces in the beam (s_hlSkip), and the flush
// draws those pieces once through scenehl with the whole sum, writing depth
// like any opaque draw (hlFull). Still one program switch pair a frame.
// #395 had left the inline path because the hardware lost batches on it;
// #416 since found why (the first draw after a program change, the program
// applied lazily) and bindProg() primes every change, and these records go
// down in one run after a single switch. Wet (scenerf) and blended batches
// keep the additive overlay. g_hlOnePass 0 (config DbgBeamOnePass=0) is the
// two-pass A/B; the look sweep's beam2pass step times it.
// #442 (2026-11-08): OFF by default. On the user's New 3DS the road under the
// beam glitched out now and then (the leftover of #416), and DbgBeamOnePass=0
// cured it: the 3DS still loses an occasional scenehl draw, which in one pass
// is the road itself, in two passes only the beam's added light. One pass
// stays for DbgBeamOnePass=1 (it saved ~1-5 ms of GPU).
u32 g_hlOnePass = 0;
static bool s_hlTwoPassStep = false;   // the look sweep's beam2pass step

void model3d_beam_two_pass(bool on)
{
	s_hlTwoPassStep = on;
}

u32 model3d_headlight_full(void)
{
	return s_hlFullBatches;
}

// The one-pass records among r[0..n), drawn as opaque batches: blend off,
// depth written, the real fog colour. Switches to scenehl only when there is
// one (*prog follows); returns what is on unit 1, as hlOverlay() does.
static const C3D_Tex* hlFull(const HlRec* r, int n, int* prog, const C3D_Tex* diffuse)
{
	int k = 0;
	while (k < n && !r[k].full)
		k++;
	if (k == n)
		return diffuse;
	progSwitch(*prog, PROG_HL, diffuse);
	*prog = PROG_HL;
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	if (s_fogOn)
		C3D_FogColor(s_fogColor);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	const C3D_Tex* bound = NULL;
	int cut = -1;
	const u32 tris0 = s_drawTris;
	s_hlCull = g_hlRunCull != 0;
	const Model3D* bufM = NULL;
	for (int i = k; i < n; i++) {
		if (!r[i].full)
			continue;
		if (r[i].m != bufM) {
			C3D_SetAttrInfo((C3D_AttrInfo*)&r[i].m->attr);
			C3D_SetBufInfo((C3D_BufInfo*)&r[i].m->buf);
			bufM = r[i].m;
		}
		if (r[i].us != s_uvS || r[i].ut != s_uvT) {
			C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uUvOff, r[i].us, r[i].ut,
				0.0f, 0.0f);
			s_uvS = r[i].us;
			s_uvT = r[i].ut;
		}
		if (r[i].bind != bound) {
			C3D_TexEnvInit(env);
			hlStages(env, r[i].bind, false);
			bound = r[i].bind;
		}
		if (r[i].cut != cut) {
			C3D_AlphaTest(r[i].cut != 0, GPU_GREATER, 0x40);
			cut = r[i].cut;
		}
		drawBatchGeometry(r[i].m, r[i].b, r[i].need, r[i].cull);
		s_hlBatches++;
		s_hlFullBatches++;
	}
	s_hlCull = false;
	s_hlTris += s_drawTris - tris0;
	return bound;
}

static const C3D_Tex* hlOverlay(const Model3D* m, const HlRec* r, int n, bool blended,
		int fromProg, const C3D_Tex* fromDiffuse)
{
	// Only the additive records: the one-pass ones (r.full) are hlFull()'s,
	// which the caller runs first, unscissored -- their overlay's GEQUAL then
	// meets the depth they wrote. They may have left another model's buffers
	// bound.
	int nOver = 0;
	bool anyFull = false;
	for (int i = 0; i < n; i++) {
		nOver += !r[i].full;
		anyFull = anyFull || r[i].full;
	}
	if (!nOver)
		return fromDiffuse;
	progSwitch(fromProg, PROG_HL, fromDiffuse);
	if (s_fogOn)
		C3D_FogColor(0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, blended ? GPU_SRC_ALPHA : GPU_ONE, GPU_ONE,
		GPU_ZERO, GPU_ONE);
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	const C3D_Tex* bound = NULL;
	int cut = -1;
	const u32 tris0 = s_drawTris;
	s_hlCull = g_hlRunCull != 0;   // #412
	const Model3D* bufM = anyFull ? NULL : m;
	for (int i = 0; i < n; i++) {
		if (r[i].full)
			continue;
		// A queued record may be another section's (#412): its buffers, and
		// the scroll it was drawn with (scenehl reads uvoff where scene does).
		if (r[i].m != bufM) {
			C3D_SetAttrInfo((C3D_AttrInfo*)&r[i].m->attr);
			C3D_SetBufInfo((C3D_BufInfo*)&r[i].m->buf);
			bufM = r[i].m;
		}
		if (r[i].us != s_uvS || r[i].ut != s_uvT) {
			C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uUvOff, r[i].us, r[i].ut,
				0.0f, 0.0f);
			s_uvS = r[i].us;
			s_uvT = r[i].ut;
		}
		if (r[i].bind != bound) {
			C3D_TexEnvInit(env);
			hlStages(env, r[i].bind, true);
			bound = r[i].bind;
		}
		// #173: a blended record's overlay adds SRC_ALPHA-weighted colour and
		// writes no depth, so its alpha-0 fragments go (cutSet()).
		const int wantCut = r[i].cut ? 1 : (blended && g_blendCut) ? 2 : 0;
		if (wantCut != cut) {
			cutSet(wantCut);
			cut = wantCut;
		}
		drawBatchGeometry(r[i].m, r[i].b, r[i].need, r[i].cull);
		s_hlBatches++;
	}
	s_hlCull = false;
	s_hlTris += s_drawTris - tris0;
	return bound;
}

void model3d_beam_defer(bool on)
{
	s_hlDeferOn = on && g_hlDefer && s_hlOn;
	s_hlQn = 0;
}

// #412, the second look sweep (the 3DS, J29.1, with the beam deferred): one
// program switch pair a frame took nothing off -- the beam still cost 6 to 9
// ms of GPU. So it is its pixels: every pixel of every triangle it is laid
// over takes three texture reads and a blend, black or not, and the PICA
// tests depth after the combiners. The beam's term is zero outside the part
// of its frustum HEADLIGHTS has colour in (carlights.c, the corners), so the
// pass is scissored to that solid's box on screen: the corners clipped to
// w > 0 (each edge cut where it crosses), projected, and taken out to whole
// pixels with one to spare. Pixel-identical. The scissor is in the
// framebuffer's own frame, as the viewport C3D_FrameDrawOn() sets maps clip
// space onto it (the projection's tilt is already in `vp`): x over its
// width, y over its height. g_hlScissor 0 (config DbgBeamScissor=0) is the
// A/B; the look sweep's beamfull / beampix steps go through
// model3d_beam_scissor_mode().
u32 g_hlScissor = 1;
static int s_hlScMode = 1;
static bool s_hlScBaseTiny = false;
static u16 s_hlScRect[4];
static float s_hlScShare;

void model3d_beam_scissor_mode(int mode, bool baseTiny)
{
	s_hlScMode = mode;
	s_hlScBaseTiny = baseTiny;
}

void model3d_beam_scissor_stats(u16 outRect[4], float* outShare)
{
	if (outRect)
		memcpy(outRect, s_hlScRect, sizeof(s_hlScRect));
	if (outShare)
		*outShare = s_hlScShare;
}

// The beam's box in framebuffer pixels; false when it is the whole target
// (or the corners give nothing in front of the eye to go by).
static bool hlScreenBox(const C3D_Mtx* vp, u32 fbW, u32 fbH, u32 out[4])
{
	float c[8][4];
	for (int i = 0; i < 8; i++) {
		const float* p = s_hl.corners[i];
		for (int k = 0; k < 4; k++)
			c[i][k] = vp->r[k].x * p[0] + vp->r[k].y * p[1] + vp->r[k].z * p[2] + vp->r[k].w;
	}
	const float kW = 1e-3f;
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	int n = 0;
	#define HL_TAKE(q) do { const float iw = 1.0f / (q)[3]; \
		const float px = (q)[0] * iw, py = (q)[1] * iw; \
		x0 = px < x0 ? px : x0; x1 = px > x1 ? px : x1; \
		y0 = py < y0 ? py : y0; y1 = py > y1 ? py : y1; n++; } while (0)
	for (int i = 0; i < 8; i++)
		if (c[i][3] > kW)
			HL_TAKE(c[i]);
	// The twelve edges: corner pairs one bit apart.
	for (int i = 0; i < 8; i++)
		for (int b = 1; b < 8; b <<= 1) {
			const int j = i | b;
			if (j == i)
				continue;
			const float wi = c[i][3], wj = c[j][3];
			if ((wi > kW) == (wj > kW))
				continue;
			const float t = (kW - wi) / (wj - wi);
			float q[4];
			for (int k = 0; k < 4; k++)
				q[k] = c[i][k] + t * (c[j][k] - c[i][k]);
			q[3] = kW;
			HL_TAKE(q);
		}
	#undef HL_TAKE
	if (!n)
		return false;
	if (x0 < -1.0f) x0 = -1.0f;
	if (y0 < -1.0f) y0 = -1.0f;
	if (x1 > 1.0f) x1 = 1.0f;
	if (y1 > 1.0f) y1 = 1.0f;
	if (x1 <= x0 || y1 <= y0) {
		// Wholly off screen: an empty box is not expressible, so one pixel.
		out[0] = out[1] = 0;
		out[2] = out[3] = 1;
		return true;
	}
	const float l = floorf((x0 + 1.0f) * 0.5f * (float)fbW) - 1.0f;
	const float r = ceilf((x1 + 1.0f) * 0.5f * (float)fbW) + 1.0f;
	const float t = floorf((y0 + 1.0f) * 0.5f * (float)fbH) - 1.0f;
	const float bt = ceilf((y1 + 1.0f) * 0.5f * (float)fbH) + 1.0f;
	out[0] = l < 0.0f ? 0u : (u32)l;
	out[1] = t < 0.0f ? 0u : (u32)t;
	out[2] = r > (float)fbW ? fbW : (u32)r;
	out[3] = bt > (float)fbH ? fbH : (u32)bt;
	return !(out[0] == 0 && out[1] == 0 && out[2] == fbW && out[3] == fbH);
}

void model3d_beam_flush(const C3D_Mtx* vp)
{
	s_hlDeferOn = false;
	if (!s_hlQn)
		return;
	// The one-pass pieces first and whole: their plain draw was skipped, so
	// the scissor (the beam's colour only) must not clip them.
	int prog = PROG_SCENE;
	const C3D_Tex* d = hlFull(s_hlQ, s_hlQn, &prog, NULL);
	bool scissored = false;
	memset(s_hlScRect, 0, sizeof(s_hlScRect));
	s_hlScShare = 0.0f;
	if (s_hlScMode == 2 && !s_hlScBaseTiny) {
		C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 8, 8);
		scissored = true;
	} else if (s_hlScMode == 1 && g_hlScissor && vp && !s_hlScBaseTiny) {
		C3D_FrameBuf* fb = C3D_GetFrameBuf();
		u32 box[4];
		if (fb && fb->width && fb->height && hlScreenBox(vp, fb->width, fb->height, box)) {
			C3D_SetScissor(GPU_SCISSOR_NORMAL, box[0], box[1], box[2], box[3]);
			for (int k = 0; k < 4; k++)
				s_hlScRect[k] = (u16)box[k];
			s_hlScShare = (float)((box[2] - box[0]) * (box[3] - box[1])) /
				(float)(fb->width * fb->height);
			scissored = true;
		}
	}
	// What every section's drawBatchPass() leaves: the scene program up.
	d = hlOverlay(NULL, s_hlQ, s_hlQn, false, prog, d);
	s_hlQn = 0;
	if (scissored)
		C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	progSwitch(PROG_HL, PROG_SCENE, d);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_AlphaTest(false, GPU_ALWAYS, 0x00);
	if (s_fogOn)
		C3D_FogColor(s_fogColor);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uUvOff, 0.0f, 0.0f, 0.0f, 0.0f);
	s_uvS = s_uvT = 0.0f;
}

// Runs the batch list. `paintColor` is the constant the untextured batches are
// shaded with; pass 0xFFFFFFFF for a room, whose untextured batches want their
// baked vertex colour through unchanged.
// `cutout` picks how a textured batch's alpha is resolved: true for the
// GPU_GREATER 0x40 test the world and the cars want, false for straight
// blending, which is what the skydome cap's soft edge and the car shadow's
// falloff need (a cutout would turn either into a hard-edged disc).
// One run down the batch list, drawing only the batches whose texture material
// asks for `blendPass`. See drawBatches() for why this is split in two.
// `slotPaints`, when not NULL, overrides `paintColor` per batch by its paint
// slot (BATCH_PAINT_SLOT); a zero entry keeps `paintColor`.
static void drawBatchPass(const Model3D* m, u32 paintColor, const u32* slotPaints,
		bool cutout, bool blendPass, bool farLod) {
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	s32 boundTex = -2;   // -2 = nothing configured yet, -1 = flat colour
	u32 boundPc = 0;     // the paint constant the bound TEV was built with
	int boundBlend = -1;
	int boundDepth = 0;  // matches the WRITE_COLOR drawBatches() sets below
	bool boundPaint = false;  // TEV stages 1 and 2 are live (the tint)
	bool boundTint = false;   // the bound TEV multiplies the texture by boundPc
	// #352: the slot the plain stage (texture x vertex colour) was last built
	// for, and its cutout test. While boundTex is still that slot's stage, a
	// batch that wants the same plain stage on another texture only rebinds
	// unit 0: the stage's words and the test are what they already are.
	s32 plainTex = -3;
	int plainCut = -1;
	int nGloss = 0;           // opaque batches s_gloss holds for the second pass
	int nHl = 0;              // opaque batches s_hlRec holds for the beam (#395)
	// The projected headlight and the road's reflection (#296): world sections
	// only, in the main view.
	const bool hlAllowed = s_hlOn && m->shading == SHADING_BAKED && !s_envView &&
		s_solo < 0 && !s_feX2;
	const bool rfAllowed = s_rfOn && m->shading == SHADING_BAKED && !s_envView &&
		s_solo < 0 && !s_feX2;
	int boundProg = PROG_SCENE;          // which program and stages are up
	const C3D_Tex* progDiffuse = NULL;   // what hlStages()/rfStages() put on unit 1

	// Either this pass's slice of the pre-sorted list, or the whole list for a
	// model with no material data (the skydome, a car packed before #205) --
	// those are all opaque, so the blend pass never runs on them. Frontend
	// rooms carry their TPK materials since #105 and sort like a section.
	const int first = (m->passOrder && blendPass) ? m->numOpaque : 0;
	const int last  = m->passOrder ? (blendPass ? m->numBatches : m->numOpaque)
	                               : m->numBatches;

	for (int k = first; k < last; k++) {
		const int i = m->passOrder ? (int)m->passOrder[k] : k;
		g_walk[MODEL3D_WALK_LOOKED]++;
		// Which level of detail a batch belongs to is its position in the list
		// (see Model3D::lodNearFirst): before lodNearFirst it is drawn at every
		// distance, between the two boundaries it is the near model, and past
		// lodFarFirst it is the far one. Two compares, and no cost at all for a
		// model with no split, whose boundaries are both past the end.
		//
		// With an instance table that is per instance rather than per model
		// (#247): the batch is drawn as the runs whose instances chose its
		// level, and a level no instance chose this frame is skipped whole.
		u8 need = 0;
		if (s_solo >= 0) {
			// One instance on its own: only the batches holding it, at its
			// level, unculled -- its boxes are where it was baked.
			if (!(s_soloMask[i >> 3] & (1u << (i & 7))))
				continue;
			need = batchNeed(m, i);
			if (!(need & s_soloNeed))
				continue;
		} else if (m->runs) {
			need = batchNeed(m, i);
			if (!(need & viewAny(m)))
				continue;
		} else if (farLod ? ((i >= m->lodNearFirst && i < m->lodFarFirst) ||
		                     i >= m->envFirst)
		                  : (i >= m->lodFarFirst)) {
			continue;
		}
		const ModelBatch* b = &m->batches[i];
		if (s_turnPass != TURN_ALL &&
		    ((b->flags & BATCH_TURNTABLE) != 0) != (s_turnPass == TURN_DISC))
			continue;
		// A car's part at another level of detail than the one this draw
		// is at (MODEL3D_BATCH_LOD_*).
		if (m->carLods) {
			const u32 lm = (b->flags & MODEL3D_BATCH_LOD_MASK) >> MODEL3D_BATCH_LOD_SHIFT;
			// A rear rim turned away from the camera (s_carRimSwap, only
			// ever set at LOD A) is its part's B model alone; with no B
			// model it is not drawn, as Render draws nothing for a NULL
			// +0x1b60.
			const u32 corner = (b->flags & MODEL3D_BATCH_CORNER_MASK) >>
				MODEL3D_BATCH_CORNER_SHIFT;
			const u32 rim = !(b->flags & MODEL3D_BATCH_PART) &&
				((b->flags & MODEL3D_BATCH_WHEEL_MASK) >>
				MODEL3D_BATCH_WHEEL_SHIFT) == MODEL3D_WHEEL_RIM && corner >= 2
				? 1u << (corner - 2) : 0u;
			if (lm && (rim & s_carRimSwap)) {
				if (!(lm & 2u))
					continue;
			} else if (lm && !(lm & (1u << s_carLod))) {
				continue;
			}
		}
		// The front wheel part switched off (Model3DPartPose::noFrontWheel).
		if (s_partDraw && s_wh.parts.noFrontWheel && !(b->flags & MODEL3D_BATCH_PART) &&
		    ((b->flags & MODEL3D_BATCH_WHEEL_MASK) >> MODEL3D_BATCH_WHEEL_SHIFT) ==
		        MODEL3D_WHEEL_RIM &&
		    ((b->flags & MODEL3D_BATCH_CORNER_MASK) >> MODEL3D_BATCH_CORNER_SHIFT) < 2)
			continue;
		// A door panel, door sill or hood / trunk underside of a car whose
		// part is shut (#372 step 7, MODEL3D_BATCH_PART_COND).
		if (s_partDraw && (b->flags & MODEL3D_BATCH_PART_COND) &&
		    (b->flags & MODEL3D_BATCH_PART) && !partShows(b->flags))
			continue;
		// A trunk audio component (#314): the installed model in the
		// frontend, the EMPTY placeholder in the world (Render's
		// GameFlow 3 / showcase test).
		if (s_partDraw && (b->flags & MODEL3D_BATCH_PART_AUDIO) &&
		    (b->flags & MODEL3D_BATCH_PART) &&
		    ((b->flags & MODEL3D_BATCH_PART_SUB) != 0) != s_wh.parts.fe)
			continue;
		// A race barrier, start/finish line or shop beam whose groups are off
		// (scenery.h): DrawAScenery's skip, one test per tagged batch.
		if (m->sceneryOn) {
			const u32 set = BATCH_SCENERY_SET(b->flags);
			if (set && !(m->sceneryOn[set >> 3] & (1u << (set & 7))))
				continue;
		}
		if (!b->indexCount ||
		    b->firstIndex + b->indexCount > (u32)m->numIndices)
			continue;
		// Before anything is bound: a batch whose box is behind the camera or
		// off to the side costs one test and nothing else. This is where the
		// draw calls go down rather than up -- the cells below can only ever
		// split a batch that is already being drawn.
		int cullState = 2;
		if (s_cullOn && s_solo < 0 &&
		    ((m->cells && b->cellCount) || (m->runs && b->runCount))) {
			cullState = boxState(b->lo, b->hi);
			if (!cullState) {
				g_walk[MODEL3D_WALK_CULLED]++;
				continue;
			}
			if ((s_forceBits & DBG_FORCE_WHOLE) && cullState == 1)
				cullState = 2;
		}
		s32 ti = b->texIndex;
		if (ti >= 0 && ti >= m->numTextures)
			ti = -1;
		const C3D_Tex* bind = NULL;
		if (ti >= 0)
			bind = m->extTextures ? m->extTextures[ti]
			                      : (m->texValid[ti] ? &m->textures[ti] : NULL);
		if (!bind)
			ti = -1;
		else if (m->texAnim && m->texAnim[ti] != 0xFFFF) {
			// A texture animation's frame (#326): this frame's art is another
			// slot's. Its own until that one is resident.
			const C3D_Tex* a = animBind(m, ti);
			s_animDraws++;
			if (a) {
				bind = a;
				s_animSwaps++;
			}
		}

		// Diagnostic: isolate one texture hash. 0 draws everything.
		if (g_dbgOnlyHash && !(ti >= 0 && m->texHashes &&
		                       m->texHashes[ti] == g_dbgOnlyHash))
			continue;

		// An untextured batch has no material, so it belongs to the opaque
		// pass -- it is flat paint or baked vertex colour either way.
		const int blend = (ti >= 0 && m->texMat) ? MAT_BLEND(m->texMat[ti])
		                                         : MAT_BLEND_OPAQUE;
		if ((blend != MAT_BLEND_OPAQUE) != blendPass)
			continue;

		// #350's leftover (DbgForce): forget what this pass believes is
		// already set, so the batch writes it again.
		if (s_forceBits) {
			if (s_forceBits & DBG_FORCE_TEV) {
				boundTex = -2;
				plainTex = -3;
				plainCut = -1;
			}
			if (s_forceBits & DBG_FORCE_UV)
				s_uvS = s_uvT = -1.0e30f;
			if (s_forceBits & DBG_FORCE_BLEND) {
				boundBlend = -1;
				boundDepth = -1;
				if (!blendPass) {
					C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
						GPU_ONE, GPU_ZERO);
					C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
				}
			}
			if (s_forceBits & DBG_FORCE_UNIF)
				C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
		}

		// writeDepth is its own field in the record, not just the inverse of
		// the blend type: ten of the world's 1482 textures are additive and
		// still write depth. Honour it rather than assuming.
		int blendCut = 0;
		if (blendPass) {
			const int wantDepth = m->texMat ? MAT_DEPTH(m->texMat[ti >= 0 ? ti : 0]) : 0;
			if (ti >= 0 && wantDepth != boundDepth) {
				C3D_DepthTest(true, GPU_GREATER,
					wantDepth ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
				boundDepth = wantDepth;
			}
			// #173 (cutSet()): only where the depth state is this batch's own.
			blendCut = ti >= 0 && !wantDepth && g_blendCut;
		}

		if (blendPass && blend != boundBlend) {
			// Additive for the neon and glow art, normal blending for
			// everything else the record marks transparent. A blend type this
			// port has never seen in the shipped data is already carried as a
			// normal blend by track_pack.texture_material(), so there is no
			// third case to invent here.
			if (blend == MAT_BLEND_ADDITIVE)
				C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
					GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
			else
				C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
					GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
					GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
			// #340: an additive batch fogs to black. The fog unit mixes the
			// fragment toward the fog colour before the blend, so an additive
			// surface fogged to the 36,48,68 haze adds that haze over its
			// whole footprint, and more for each layer stacked on it. The
			// spotlight beams (SFX_LIGHT_BEAMA/B, three or four quads a beam,
			// texture alpha ~0.93 top to bottom) came out as hard-edged
			// pale-cyan wedges over the downtown towers, stronger the
			// further off they were. Fogged to black, an emissive surface
			// fades with distance and leaves the haze to what is behind
			// it, the same rule drawGloss, drawEnv and rfOverlay already
			// keep. PORT: the PC's fixed-function fog mixes its additive
			// passes toward FogColor too (SPEED2.EXE 0x5CB0B8 sets it once a
			// view, the fog colour x 0.75), but that is a mild linear fog
			// to 250 m; this one is the port's own (#148), strong enough to
			// hide the pager's 400 m edge, and at that strength the haze an
			// additive layer picks up is what shows. The colour register
			// only, never the mode or the LUT (#328); the hardware has taken
			// colour switches mid-frame since #328's fix.
			if (s_fogOn)
				C3D_FogColor(blend == MAT_BLEND_ADDITIVE ? 0u : s_fogColor);
			boundBlend = blend;
		}

		// The texture's scroll (#269), on the scene shader only -- the car
		// shader has no uvoff, and no car texture scrolls.
		if (m->shading == SHADING_BAKED) {
			float us = 0.0f, ut = 0.0f;
			if (ti >= 0 && m->texMat && MAT_SCROLL(m->texMat[ti]) && m->texHashes) {
				const TexScroll* e = scrollFor(m->texHashes[ti]);
				if (e) {
					us = e->s;
					ut = e->t;
				}
			}
			if (us != s_uvS || ut != s_uvT) {
				C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uUvOff, us, ut, 0.0f, 0.0f);
				s_uvS = us;
				s_uvT = ut;
			}
		}

		// The projected headlight (#296): a textured batch that may be in the
		// beam goes through scenehl.v.pica. One with no box of its own is
		// taken as in it -- the beam is black outside its frustum, so that
		// costs time and never a pixel.
		const bool hlSel = hlAllowed && ti >= 0 && !g_dbgHashTint &&
			(!((m->cells && b->cellCount) || (m->runs && b->runCount)) ||
			 hlBoxIn(b->lo, b->hi));
		// #395: drawn plain (or wet) and the beam laid over it by hlOverlay();
		// inline through scenehl only for DbgNoBeam=3 (model3d_set_beam_inline).
		const bool hlLater = hlSel && !s_hlInline && m->hasMaterials;
		const bool hl = hlSel && !hlLater;
		// The road's reflection (#296): a textured batch with retail's 0x40
		// goes through scenerf.v.pica. One in the beam too is the beam's here,
		// and rfOverlay() lays the capture over it below -- in the opaque pass
		// only (PORT: a blended one keeps the beam alone).
		const bool rf = rfAllowed && ti >= 0 && !g_dbgHashTint &&
			(b->flags & BATCH_REFLECTIVE);
		const int prog = hl ? PROG_HL : rf ? PROG_RF : PROG_SCENE;
		if (s_dumpLeft > 0 && m->shading == SHADING_BAKED && !s_envView)
			dumpBatch(m, i, b, ti, prog, rf && hl && !blendPass, blendPass, cullState);
		if (prog != boundProg) {
			progSwitch(boundProg, prog, progDiffuse);
			boundProg = prog;
			boundTex = -2;
		}

		const bool skinTex = ti >= 0 && m->texSkin && m->texSkin[ti];
		u32 pc = paintColor;
		// A rim or caliper under its component paint (MODEL3D_BATCH_PAINT_TINT)
		// is its texture, lit, times that paint -- and only when the slot has
		// a paint of its own: the body colour is never what such a batch takes.
		bool tint = false;
		if (slotPaints) {
			const u32 ps = BATCH_PAINT_SLOT(b->flags);
			if (ps && ps < MODEL3D_CAR_PAINT_SLOTS && slotPaints[ps]) {
				pc = slotPaints[ps];
				// The skin already carries every component colour retail
				// paints into it (carskin.h), so a batch sampling it is never
				// tinted again.
				tint = ti >= 0 && !skinTex &&
				       (b->flags & MODEL3D_BATCH_PAINT_TINT);
			}
		}
		if (g_dbgHashTint) {
			const u32 hh = (ti >= 0 && m->texHashes) ? m->texHashes[ti] : 0;
			C3D_TexEnvInit(env);
			C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
			// C3D_TexEnvColor takes 0xAABBGGRR.
			C3D_TexEnvColor(env, 0xFF000000u | (((hh >> 8) & 0xFFu) << 16) |
				(((hh >> 16) & 0xFFu) << 8) | ((hh >> 24) & 0xFFu));
			C3D_SetTexEnv(0, env);
			C3D_AlphaTest(false, GPU_ALWAYS, 0x00);
			boundTex = -2;
			boundPaint = false;
			drawBatchGeometry(m, b, need, cullState);
			continue;
		}

		if (s_shDraw)
			shSetMaterial(m->lightMat[i]);

		// The paint constant is part of the TEV state too: two batches on one
		// texture still need a rebuild when their paint slots differ.
		const bool plain = ti >= 0 && !hl && !rf && !tint && !boundPaint;
		if (plain && g_tevKeep && boundTex >= 0 && boundTex == plainTex &&
		    ti != boundTex) {
			C3D_TexBind(0, (C3D_Tex*)bind);
			if (g_tevKeep == 2)
				C3D_DirtyTexEnv(env);
			const int wantCut = ((m->hasMaterials
				? (MAT_USAGE(m->texMat[ti]) == MAT_USAGE_PUNCHTHROUGH) : cutout) && !blendPass)
				? 1 : blendCut ? 2 : 0;
			if (wantCut != plainCut) {
				cutSet(wantCut);
				plainCut = wantCut;
			}
			boundTex = plainTex = ti;
			boundTint = false;
		} else if (ti != boundTex || tint != boundTint ||
		    ((ti < 0 || tint) && pc != boundPc)) {
			g_walk[MODEL3D_WALK_TEV]++;
			boundPc = pc;
			plainTex = -3;
			C3D_TexEnvInit(env);
			// Stages 1 and 2 belong to the tint alone;
			// anything drawn after one has to get the pass-through back or it
			// keeps multiplying its rim colour into whatever is bound now.
			if (boundPaint) {
				C3D_TexEnv* e = C3D_GetTexEnv(1);
				C3D_TexEnvInit(e);
				C3D_SetTexEnv(1, e);
				e = C3D_GetTexEnv(2);
				C3D_TexEnvInit(e);
				C3D_SetTexEnv(2, e);
				boundPaint = false;
			}
			if (ti >= 0) {
				if (hl) {
					hlStages(env, bind, false);
					progDiffuse = bind;
				} else if (rf) {
					rfStages(env, bind);
					progDiffuse = bind;
				} else {
					C3D_TexBind(0, (C3D_Tex*)bind);
					C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR,
						GPU_PRIMARY_COLOR);
					C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
						GPU_TEVOP_RGB_SRC_COLOR);
					C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
					if (!tint)
						plainTex = ti;
				}
				// Decals, badges, light lenses, chain-link fence panels and
				// posters are alpha cutouts. The batches are in retail file
				// order, not sorted back to front, so a cutout test is the
				// honest choice over blending here.
				//
				// It is off in the blended pass: those surfaces are being
				// blended precisely because their coverage is gradual, so a
				// hard 0x40 threshold would punch holes in the soft edge of
				// every glow instead of fading it.
				//
				// And it is off for a surface whose alpha is not coverage at
				// all. The old comment here claimed "for world art the TPK
				// record's own alphaUsageType says so outright" -- it does,
				// and this code was not reading it: `cutout` was one boolean
				// for the whole model, so every textured batch in the opaque
				// pass got cut at 0x40 regardless of what its material said.
				//
				// That was visible, and badly. The main road surface at the
				// career spawn (05dbf754, 917 m2 of it in C32) is declared
				// alphaUsageType 2, "real alpha", with blend type opaque, and
				// its alpha histogram is a smooth bell centred on 7/15 -- a
				// grunge/blend mask, not coverage, which is bimodal. Cutting
				// it at 0x40 discarded 12.2% of its texels and punched the
				// road full of holes; the second ground texture beside it
				// (ae306e88) lost 78.6%. Across L4RA 160257 triangles are in
				// that class, 4.4% of the world, concentrated in exactly the
				// surfaces the camera is closest to.
				//
				// So: cut only what the TPK calls punchthrough, which is what
				// punchthrough means. usage 2 with an opaque blend keeps its
				// RGB and ignores its alpha, and usage 0 has no alpha to read.
				// A model with no material data at all -- the skydome, a car
				// packed before #205 -- cannot say, so it keeps the caller's
				// boolean and behaves exactly as before. (Frontend rooms carry
				// their TPK materials since #105.)
				//
				// Cars were the other casualty of the single boolean (#205):
				// their headlight and brakelight lenses are additive glass
				// over an opaque lamp, and a cutout drew the lens's alpha-136
				// black as a solid black lid over it.
				const bool wantCut = m->hasMaterials
					? (MAT_USAGE(m->texMat[ti]) == MAT_USAGE_PUNCHTHROUGH)
					: cutout;
				plainCut = (wantCut && !blendPass) ? 1 : blendCut ? 2 : 0;
				cutSet(plainCut);
			} else {
				C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
				C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
					GPU_TEVOP_RGB_SRC_COLOR);
				C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
				C3D_TexEnvColor(env, pc);
				C3D_AlphaTest(false, GPU_ALWAYS, 0x00);
			}
			// The SH car's CLR0 is stored at half (sat(v * 128)), and mode 7
			// takes it back at x2 -- the alpha too, which is 2 tex.a CLR0.a.
			if (s_shDraw)
				C3D_TexEnvScale(env, C3D_Both, GPU_TEVSCALE_2);
			else if (s_feX2)
				C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
			C3D_SetTexEnv(0, env);
			if (tint) {
				// One more stage: previous * paint, alpha passed through so
				// the cutout test above still sees the texture's coverage.
				C3D_TexEnv* e1 = C3D_GetTexEnv(1);
				C3D_TexEnvInit(e1);
				C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
				C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
					GPU_TEVOP_RGB_SRC_COLOR);
				C3D_TexEnvFunc(e1, C3D_RGB, GPU_MODULATE);
				C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
				C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
				C3D_TexEnvColor(e1, pc | 0xFF000000);
				C3D_SetTexEnv(1, e1);
				boundPaint = true;
			}
			boundTex = ti;
			boundTint = tint;
		}

		g_walk[MODEL3D_WALK_DRAWN]++;
		// The disc shadow (#105): the vertex alpha out of the colour slot,
		// and the TEV's modulate takes texture alpha x it.
		const bool vAlpha = s_feX2 && (b->flags & BATCH_FE_VALPHA) &&
			m->shading == SHADING_BAKED;
		if (vAlpha)
			C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uShade, s_shadeNow[0],
				s_shadeNow[1], s_shadeNow[2], 1.0f);
		// #412 one pass (g_hlOnePass): an opaque, dry, untinted batch in the
		// beam leaves its beam pieces to the flush, which draws them whole.
		const bool hlOne = hlLater && g_hlOnePass && !s_hlTwoPassStep && s_hlDeferOn && !blendPass && !rf &&
			!tint && !vAlpha && !s_shDraw && !s_feX2 && !s_reflView && s_solo < 0 &&
			nHl < HL_MAX;
		s_hlSkip = hlOne;
		drawBatchGeometry(m, b, need, cullState);
		s_hlSkip = false;
		if (vAlpha)
			C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uShade, s_shadeNow[0],
				s_shadeNow[1], s_shadeNow[2], 0.0f);
		if (hl)
			s_hlBatches++;
		if (rf && !hl) {
			s_rfBatches++;
		} else if (rf && !blendPass && s_wetProbe != 1) {
			// Over the beam's pixels; it leaves its own stages and program up.
			rfOverlay(m, b, need, cullState, bind);
			s_rfBatches++;
			boundProg = PROG_RF;
			progDiffuse = bind;
			boundTex = -2;
		}
		if (hlLater) {
			const HlRec r = { .m = m, .b = b, .bind = bind, .us = s_uvS, .ut = s_uvT,
				.cut = (u8)(!blendPass &&
					MAT_USAGE(m->texMat[ti]) == MAT_USAGE_PUNCHTHROUGH),
				.need = need, .cull = (u8)cullState, .full = (u8)hlOne };
			if (!blendPass && nHl < HL_MAX) {
				s_hlRec[nHl++] = r;
			} else {
				// A blended batch's beam goes on at once, before anything else
				// blends over it; an opaque one past HL_MAX too.
				progDiffuse = hlOverlay(m, &r, 1, blendPass, boundProg, progDiffuse);
				boundProg = PROG_HL;
				boundTex = -2;
				plainTex = -3;
				plainCut = -1;
				if (s_fogOn)
					C3D_FogColor(s_fogColor);
				if (blendPass) {
					boundBlend = -1;
					boundDepth = -1;
				} else {
					C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE,
						GPU_ZERO);
					C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
				}
			}
		}

		// The SH car's gloss (#166): over a blended batch at once, before
		// anything else blends over it, and over the opaque ones in one run
		// once they are all down.
		if (s_shDraw) {
			const GlossRec r = { b, ti >= 0 ? bind : NULL, m->lightMat[i],
				(u8)(ti >= 0 && !blendPass && (m->hasMaterials
					? MAT_USAGE(m->texMat[ti]) == MAT_USAGE_PUNCHTHROUGH : cutout)),
				need, (u8)cullState };
			if (blendPass) {
				resetPaintStages(&boundPaint);
				drawGloss(m, &r, 1, true);
				drawEnv(m, &r, 1, true);
				boundTex = -2;
				boundBlend = -1;
				boundDepth = -1;
			} else if (nGloss < GLOSS_MAX) {
				s_gloss[nGloss++] = r;
			}
		}
	}

	// The beam over the section's opaque batches, all at once (#395), or
	// queued for the whole world's (#412).
	if (nHl && s_hlDeferOn && s_hlQn + nHl <= HLQ_MAX) {
		memcpy(&s_hlQ[s_hlQn], s_hlRec, sizeof(HlRec) * (size_t)nHl);
		s_hlQn += nHl;
	} else if (nHl) {
		progDiffuse = hlFull(s_hlRec, nHl, &boundProg, progDiffuse);
		progDiffuse = hlOverlay(m, s_hlRec, nHl, false, boundProg, progDiffuse);
		boundProg = PROG_HL;
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		if (s_fogOn)
			C3D_FogColor(s_fogColor);
	}

	// The haze back for whatever is drawn next (#340).
	if (blendPass && boundBlend == MAT_BLEND_ADDITIVE && s_fogOn)
		C3D_FogColor(s_fogColor);

	// The SH car's passes, one by one in a traced frame (gpuwatch.h).
	if (s_shDraw)
		gpuwatch_trace(blendPass ? "car: blended batches, each with its gloss + reflection"
			: "car: opaque batches");
	if (nGloss) {
		resetPaintStages(&boundPaint);
		if (drawGlossEnv(m, s_gloss, nGloss)) {
			gpuwatch_trace(s_envNoSphere
				? "car: gloss + reflection in one pass, the sphere off (unit 1 off)"
				: "car: gloss + reflection in one pass over the opaque");
		} else {
			drawGloss(m, s_gloss, nGloss, false);
			gpuwatch_trace("car: gloss over the opaque");
			drawEnv(m, s_gloss, nGloss, false);
			gpuwatch_trace(s_envNoSphere ? "car: reflection, the shader alone (unit 1 off)"
				: "car: reflection over the opaque");
		}
		// What drawBatches() had up for the opaque pass.
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	}
	progSwitch(boundProg, PROG_SCENE, progDiffuse);

	C3D_AlphaTest(false, GPU_ALWAYS, 0x00);
	// Leave the TEV the way every other draw in the frame expects to find it.
	// A car is drawn in the middle of a world frame, so a paint batch that
	// happened to be last would otherwise hand the next model two live stages.
	if (boundPaint) {
		C3D_TexEnv* e = C3D_GetTexEnv(1);
		C3D_TexEnvInit(e);
		C3D_SetTexEnv(1, e);
		e = C3D_GetTexEnv(2);
		C3D_TexEnvInit(e);
		C3D_SetTexEnv(2, e);
	}
}

// Runs the batch list, opaque surfaces first and the transparent ones after.
//
// The split is what makes the transparent art read correctly at all. The
// batches are in retail file order, which is a material order and has nothing
// to do with depth, so a neon barrier drawn before the road behind it used to
// blend against whatever happened to be in the colour buffer and, because it
// wrote depth, then hid the road it should have been glowing in front of.
// Drawing every opaque surface first means the transparent ones always blend
// against a finished scene, and they write no depth so they cannot hide each
// other. Additive is order-independent, and the neon and glow art -- the great
// majority of what is transparent out here -- is all additive.
//
// A model whose textures are all opaque (every frontend room, the skydome and
// the shadow quad, none of which carries material data) takes the first pass
// only and is drawn exactly as before, with whatever depth and blend state
// setCommonState() chose for its pass. A car carries material data (#205): its
// lens glass, badges and window tint go in the second pass, after its body.
static void drawBatches(const Model3D* m, u32 paintColor, const u32* slotPaints,
		bool cutout, Model3DPass pass, bool farLod, Model3DPhase phase) {
	// A model that carries material data gets a genuinely opaque first pass.
	// setCommonState() leaves straight alpha blending up for every draw, which
	// the material-less models rely on (the skydome cap's soft edge, the car
	// shadow's falloff), and the TEV passes texture alpha through -- so until
	// now every world surface in the "opaque" pass was blended by its own
	// texture alpha.
	//
	// For most of the world that alpha is 1 and nothing showed. The road is
	// the exception, and the one that matters most (TASKS.md #191): its
	// surfaces are alphaUsageType 2 with blend type opaque, and that alpha is
	// the wet-road mask -- C33's crosswalk texture c38d3f54 has 91% of its
	// texels at 4/15 or less. Blended, most of the asphalt was 73-100%
	// see-through: the crosswalk and lane lines all but vanished and the
	// road took the colour of whatever lay under it -- the fog-blue clear
	// colour, or, before the retail visibility lists, A99's block-out ground
	// and buildings, which is the pale wash and the "building in the middle
	// of the road".
	//
	// Blend type opaque means not blended at all, so it is ONE/ZERO here. The
	// punchthrough test in drawBatchPass() still reads the TEV alpha, which is
	// what retail punchthrough is: a test, then an opaque write.
	//
	// The instances' levels first, once a frame for both phases. The state is
	// a per-frame cache the draw owns, not part of what the model is, so it is
	// written through a const model the way a mutable member would be.
	// An env view draws a section's reflection models through its instance
	// table and nothing at all of a model without one.
	if (s_envView && !m->envState)
		return;
	if (s_reflView && !m->reflTable)
		return;
	if (m->runs)
		updateInstances((Model3D*)m);
	s_forceBits = (pass == MODEL3D_PASS_WORLD && m->hasMaterials && !s_feX2 &&
		!s_envView && s_solo < 0) ? g_dbgForce : 0;
	if (phase != MODEL3D_PHASE_BLENDED) {
		if (m->hasMaterials)
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
				GPU_ONE, GPU_ZERO);
		drawBatchPass(m, paintColor, slotPaints, cutout, false, farLod);
	}
	if (phase == MODEL3D_PHASE_OPAQUE || !m->anyBlended || g_dbgNoBlend) {
		// Hand the blend state back the way the next caller expects it.
		if (m->hasMaterials)
			setCommonState(pass);
		return;
	}
	// Depth still tested -- a glow behind a building is hidden by it -- but
	// not written, which is the record's own writeDepth for every one of
	// these (see MAT_DEPTH).
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	drawBatchPass(m, paintColor, slotPaints, cutout, true, farLod);
	// Put the depth and blend state back, because this ran last: the next
	// caller in the frame may be another section (which sets its own state
	// and would be fine), but it may equally be citro2d drawing the HUD, and
	// leaving an additive blend up would make the whole overlay glow.
	setCommonState(pass);
}

void model3d_camera(C3D_Mtx* out, const float eye[3], const float target[3],
		float fovDegrees, float aspect, float nearZ, float farZ) {
	// World up is +Z: that is the axis GEOMETRY.BIN is authored against, so
	// the camera is expressed directly in retail world units rather than in a
	// rotated intermediate space.
	static const float zUp[3] = { 0.0f, 0.0f, 1.0f };
	model3d_camera_up(out, eye, target, zUp, fovDegrees, aspect, nearZ, farZ);
}

void model3d_camera_up(C3D_Mtx* out, const float eye[3], const float target[3],
		const float up[3], float fovDegrees, float aspect, float nearZ, float farZ) {
	C3D_Mtx proj, view;
	Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(fovDegrees), aspect, nearZ, farZ, false);
	Mtx_LookAt(&view, FVec3_New(eye[0], eye[1], eye[2]),
		FVec3_New(target[0], target[1], target[2]), FVec3_New(up[0], up[1], up[2]), false);
	Mtx_Multiply(out, &proj, &view);
}

void model3d_camera_up_stereo(C3D_Mtx* out, const float eye[3], const float target[3],
		const float up[3], float fovDegrees, float aspect, float nearZ, float farZ,
		float iod, float screen) {
	C3D_Mtx proj, view;
	Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(fovDegrees), aspect, nearZ, farZ, iod,
		screen, false);
	Mtx_LookAt(&view, FVec3_New(eye[0], eye[1], eye[2]),
		FVec3_New(target[0], target[1], target[2]), FVec3_New(up[0], up[1], up[2]), false);
	Mtx_Multiply(out, &proj, &view);
}

static void drawBakedLod(const Model3D* m, const C3D_Mtx* mvp, float ambient,
		float gain, float invGamma, Model3DPass pass, bool farLod,
		Model3DPhase phase);

void model3d_draw_scene(const Model3D* m, const C3D_Mtx* vp, float ambient,
		float gain, float invGamma) {
	drawBakedLod(m, vp, ambient, gain, invGamma, MODEL3D_PASS_WORLD, false,
		MODEL3D_PHASE_ALL);
}

void model3d_draw_scene_lod(const Model3D* m, const C3D_Mtx* vp, float ambient,
		float gain, float invGamma, bool farLod) {
	drawBakedLod(m, vp, ambient, gain, invGamma, MODEL3D_PASS_WORLD, farLod,
		MODEL3D_PHASE_ALL);
}

void model3d_draw_scene_lod_phase(const Model3D* m, const C3D_Mtx* vp,
		float ambient, float gain, float invGamma, bool farLod,
		Model3DPhase phase) {
	drawBakedLod(m, vp, ambient, gain, invGamma, MODEL3D_PASS_WORLD, farLod,
		phase);
}

static bool indexBox(const Model3D* m, u32 first, u32 count, float lo[3],
	float hi[3]);

// Looks for the room's turntable once: the box over every BATCH_TURNTABLE
// batch, whose middle it turns about.
static void findTurntable(Model3D* m) {
	float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	bool any = false;
	for (int i = 0; i < m->numBatches; i++) {
		const ModelBatch* b = &m->batches[i];
		float blo[3], bhi[3];
		if (!(b->flags & BATCH_TURNTABLE) || !b->indexCount ||
		    b->firstIndex + b->indexCount > (u32)m->numIndices ||
		    !indexBox(m, b->firstIndex, b->indexCount, blo, bhi))
			continue;
		for (int k = 0; k < 3; k++) {
			if (blo[k] < lo[k]) lo[k] = blo[k];
			if (bhi[k] > hi[k]) hi[k] = bhi[k];
		}
		any = true;
	}
	m->turnState = any ? 2 : 1;
	m->turnPivot[0] = 0.5f * (lo[0] + hi[0]);
	m->turnPivot[1] = 0.5f * (lo[1] + hi[1]);
}

void model3d_draw_scene_turned(const Model3D* m, const C3D_Mtx* vp, float yaw,
		float ambient, float gain, float invGamma) {
	if (!m)
		return;
	if (!m->turnState)
		findTurntable((Model3D*)m);
	s_feX2 = true;
	if (m->turnState != 2) {
		model3d_draw_scene(m, vp, ambient, gain, invGamma);
		s_feX2 = false;
		C3D_TexEnvScale(C3D_GetTexEnv(0), C3D_RGB, GPU_TEVSCALE_1);
		return;
	}
	// The disc's vertices are in room space like the rest, so it is turned
	// about its own centre: vp * T(pivot) * Rz(yaw) * T(-pivot).
	C3D_Mtx model, turned;
	Mtx_Identity(&model);
	Mtx_Translate(&model, m->turnPivot[0], m->turnPivot[1], 0.0f, true);
	Mtx_RotateZ(&model, yaw, true);
	Mtx_Translate(&model, -m->turnPivot[0], -m->turnPivot[1], 0.0f, true);
	Mtx_Multiply(&turned, vp, &model);
	// The two-phase split across room and disc, as world_draw() runs it
	// across sections: everything opaque, then everything blended.
	s_turnPass = TURN_ROOM;
	model3d_draw_scene_lod_phase(m, vp, ambient, gain, invGamma, false,
		MODEL3D_PHASE_OPAQUE);
	s_turnPass = TURN_DISC;
	model3d_draw_scene_lod_phase(m, &turned, ambient, gain, invGamma, false,
		MODEL3D_PHASE_OPAQUE);
	if (model3d_has_blended(m)) {
		s_turnPass = TURN_ROOM;
		model3d_draw_scene_lod_phase(m, vp, ambient, gain, invGamma, false,
			MODEL3D_PHASE_BLENDED);
		s_turnPass = TURN_DISC;
		model3d_draw_scene_lod_phase(m, &turned, ambient, gain, invGamma, false,
			MODEL3D_PHASE_BLENDED);
	}
	s_turnPass = TURN_ALL;
	s_feX2 = false;
	C3D_TexEnvScale(C3D_GetTexEnv(0), C3D_RGB, GPU_TEVSCALE_1);
}

void model3d_draw_scene_part(const Model3D* m, const C3D_Mtx* vp, float yaw,
		float ambient, float gain, float invGamma, Model3DScenePart part) {
	if (!m)
		return;
	if (!m->turnState)
		findTurntable((Model3D*)m);
	if (m->turnState != 2) {
		if (part == MODEL3D_SCENE_ROOM) {
			s_roomCull = true;
			model3d_draw_scene_turned(m, vp, 0.0f, ambient, gain, invGamma);
			s_roomCull = false;
		}
		return;
	}
	C3D_Mtx model, turned;
	const C3D_Mtx* use = vp;
	if (part == MODEL3D_SCENE_DISC) {
		Mtx_Identity(&model);
		Mtx_Translate(&model, m->turnPivot[0], m->turnPivot[1], 0.0f, true);
		Mtx_RotateZ(&model, yaw, true);
		Mtx_Translate(&model, -m->turnPivot[0], -m->turnPivot[1], 0.0f, true);
		Mtx_Multiply(&turned, vp, &model);
		use = &turned;
	}
	s_feX2 = true;
	s_turnPass = part == MODEL3D_SCENE_DISC ? TURN_DISC : TURN_ROOM;
	s_roomCull = part == MODEL3D_SCENE_ROOM;
	model3d_draw_scene_lod_phase(m, use, ambient, gain, invGamma, false,
		MODEL3D_PHASE_OPAQUE);
	if (model3d_has_blended(m))
		model3d_draw_scene_lod_phase(m, use, ambient, gain, invGamma, false,
			MODEL3D_PHASE_BLENDED);
	s_roomCull = false;
	s_turnPass = TURN_ALL;
	s_feX2 = false;
	C3D_TexEnvScale(C3D_GetTexEnv(0), C3D_RGB, GPU_TEVSCALE_1);
}

bool model3d_has_blended(const Model3D* m) {
	return m && m->anyBlended && !g_dbgNoBlend;
}

bool model3d_has_lod(const Model3D* m) {
	return m && m->lodFarFirst < m->envFirst;
}

bool model3d_has_env(const Model3D* m) {
	return m && m->envState;
}

bool model3d_env_any(const Model3D* m) {
	if (!m || !m->envState || !m->runs || !s_envView)
		return false;
	updateInstances((Model3D*)m);
	return m->envAny != 0;
}

void model3d_draw_baked(const Model3D* m, const C3D_Mtx* mvp, float ambient,
		float gain, float invGamma, Model3DPass pass) {
	drawBakedLod(m, mvp, ambient, gain, invGamma, pass, false, MODEL3D_PHASE_ALL);
}

bool model3d_find_instance(const Model3D* m, const float pos[3], u32* outInst) {
	if (!m || !m->insts)
		return false;
	for (u32 i = 0; i < m->numInsts; i++) {
		const float* p = m->insts[i].pos;
		if (fabsf(p[0] - pos[0]) < 0.01f && fabsf(p[1] - pos[1]) < 0.01f &&
		    fabsf(p[2] - pos[2]) < 0.01f) {
			*outInst = i;
			return true;
		}
	}
	return false;
}

u32 model3d_lod_frame(void) {
	return s_lodFrame;
}

int model3d_instance_lod(const Model3D* m, u32 inst, u32 frame) {
	if (!m || !m->instState || inst >= m->numInsts || m->instFrame != frame)
		return 0;
	const u8 st = m->instState[inst];
	return (st & INST_DRAW_A) ? 1 : (st & INST_DRAW_B) ? 2 : 0;
}

void model3d_hide_instance(Model3D* m, u32 inst, bool hide) {
	if (!m || !m->insts || inst >= m->numInsts)
		return;
	if (!m->hidden) {
		if (!hide)
			return;
		m->hidden = (u8*)calloc((m->numInsts + 7) / 8, 1);
		if (!m->hidden)
			return;
	}
	const u8 bit = (u8)(1u << (inst & 7));
	if (hide)
		m->hidden[inst >> 3] |= bit;
	else
		m->hidden[inst >> 3] &= (u8)~bit;
	// The states are a once-a-frame cache: work them out again.
	m->instFrame = 0;
	m->envFrame = 0;
}

int model3d_batch_count(const Model3D* m) {
	return m ? m->numBatches : 0;
}

bool model3d_instance_batches(const Model3D* m, u32 inst, u8* mask) {
	if (!m || !m->runs || inst >= m->numInsts)
		return false;
	memset(mask, 0, ((size_t)m->numBatches + 7) / 8);
	bool any = false;
	for (int i = 0; i < m->numBatches; i++) {
		const ModelBatch* b = &m->batches[i];
		const ModelRun* r = m->runs + b->runFirst;
		for (u32 k = 0; k < b->runCount; k++)
			if (r[k].inst == inst) {
				mask[i >> 3] |= (u8)(1u << (i & 7));
				any = true;
				break;
			}
	}
	return any;
}

void model3d_draw_instance(const Model3D* m, u32 inst, const u8* mask,
		const float pos[3], const C3D_Mtx* mvp, float ambient, float gain,
		float invGamma) {
	if (!m || !m->runs || !mask || inst >= m->numInsts || s_envView)
		return;
	// Its level where it is now, by the pick every instance takes.
	ModelInstance at = m->insts[inst];
	memcpy(at.pos, pos, sizeof(at.pos));
	s_soloNeed = instancePick(&at);
	if (!s_soloNeed)
		return;
	s_solo = (s32)inst;
	s_soloMask = mask;
	drawBakedLod(m, mvp, ambient, gain, invGamma, MODEL3D_PASS_WORLD, false,
		MODEL3D_PHASE_ALL);
	s_solo = -1;
	s_soloMask = NULL;
}

// The whole of the baked path; `farLod` picks which of the two detail runs the
// batch loop keeps (see drawBatchPass). The skydome and the shadow quad carry
// no split and are unaffected by it either way.
static void drawBakedLod(const Model3D* m, const C3D_Mtx* mvp, float ambient,
		float gain, float invGamma, Model3DPass pass, bool farLod,
		Model3DPhase phase) {
	if (!m || m->shading != SHADING_BAKED || !s_sceneShader.ready)
		return;
	const bool wasFogged = s_fogOn;

	bindProg(&s_sceneShader.program);
	C3D_SetAttrInfo((C3D_AttrInfo*)&m->attr);
	C3D_SetBufInfo((C3D_BufInfo*)&m->buf);

	// Scene vertices are already in world space -- scene_pack.py folded each
	// part's translation in -- so the view-projection is the whole transform.
	{
		C3D_Mtx mo;
		originFold(&mo, mvp);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_sceneShader.uMvp, &mo);
	}
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uShade, ambient, gain, invGamma, 0.0f);
	s_shadeNow[0] = ambient;
	s_shadeNow[1] = gain;
	s_shadeNow[2] = invGamma;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uUvOff, 0.0f, 0.0f, 0.0f, 0.0f);
	if (s_originOn)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uPosOff, s_origin[0], s_origin[1],
			s_origin[2], 0.0f);
	else
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_sceneShader.uPosOff, 0.0f, 0.0f, 0.0f, 0.0f);
	if (s_dumpLeft > 0)
		s_dumpVp = *mvp;
	s_uvS = s_uvT = 0.0f;
	s_reflRow[0] = s_reflRow[1] = s_reflRow[3] = 0.0f;
	s_reflRow[2] = 1.0f;

	setCommonState(pass);
	if (pass == MODEL3D_PASS_SKY)
		model3d_fog_enable(false);
	drawBatches(m, m->untexPaint, NULL, pass == MODEL3D_PASS_WORLD, pass, farLod,
		phase);
	if (pass == MODEL3D_PASS_SKY && wasFogged)
		model3d_fog_enable(true);
}

// The batches of a car, whichever program is bound.
static void drawCarBatches(const Model3D* m) {
	setCommonState(MODEL3D_PASS_WORLD);
	s_carLod = s_nextCarLod;
	s_nextCarLod = 0;
	if (m->ownPaint)
		drawBatches(m, m->ownPaintBody, m->ownSlotPaintAny ? m->ownSlotPaint : NULL, true,
			MODEL3D_PASS_WORLD, false, MODEL3D_PHASE_ALL);
	else
		drawBatches(m, s_carPaint, s_carSlotPaintAny ? s_carSlotPaint : NULL, true,
			MODEL3D_PASS_WORLD, false, MODEL3D_PHASE_ALL);
	s_carLod = 0;
	s_carRimSwap = 0;
	s_whOn = false;
	s_partDraw = false;
}

// TireFace (Speed.elf 0x802376c0, SPEED2.EXE 0x60d700) for the two rear rims
// of a car drawn in the world: en = (0, 1, 0) through the rim's matrix,
// dotted with the camera's direction (Camera+0x50), against FaceCos -- 0 in
// both builds (0x8046eec0, PC [0x8a1c5c] in .bss; TireFaceIt is 1, so the
// test is live). Render asks it only in game (GameFlow 6, which the world
// is: the frontend draws with model3d_draw_car) and only when the wheels'
// level, the car's clamped to C, is A (0x8023d528).
static u8 rimSwap(const Model3D* m, const float rot[3][3]) {
	if (!m->carLods || !m->rimAxes || s_nextCarLod != 0 || !s_carViewSet ||
	    g_dbgNoRimSwap == 1)
		return 0;
	if (g_dbgNoRimSwap == 2 && ((osGetTime() / 1000u) & 1u))
		return 0;
	if (g_dbgNoRimSwap == 3)
		return 3;
	u8 swap = 0;
	for (int j = 0; j < 2; j++) {
		const float* a = m->rimAxis[j];
		float d = 0.0f;
		for (int c = 0; c < 3; c++)
			d += (a[0] * rot[0][c] + a[1] * rot[1][c] + a[2] * rot[2][c]) * s_carFwd[c];
		if (d < 0.0f)
			swap |= (u8)(1u << j);
	}
	return swap;
}

// CarPartCuller part 10, CULLABLE_CAR_PART_UNDERNEATH (CullParts, Speed.elf
// 0x80230550; the plane table SPEED2.EXE 0x803ae0 + 10 * 0x50): type 1, so
// the part is culled as soon as the camera, in car space and taken from the
// part's position, is on or above either plane (dot - d >= 0, the threshold
// 0x8033b720 being 0). The position is the average of the four wheel records
// (the constructor, 0x802314c0), whose x and z the pivots carry; the planes
// have no y.
bool model3d_car_underside_seen(const Model3D* m, const float rot[3][3],
		const float origin[3]) {
	static const float kN[2][3] = { { -0.07f, 0.0f, 1.0f }, { 0.07f, 0.0f, 1.0f } };
	static const float kD = 0.05f;
	if (!m || !m->wheelPivots || !s_carViewSet)
		return false;
	float rel[3];
	for (int a = 0; a < 3; a++) {
		const float c = 0.25f * (m->wheelPivot[0][a] + m->wheelPivot[1][a] +
		                         m->wheelPivot[2][a] + m->wheelPivot[3][a]);
		rel[a] = rot[a][0] * (s_carEye[0] - origin[0]) + rot[a][1] * (s_carEye[1] - origin[1]) +
		         rot[a][2] * (s_carEye[2] - origin[2]) - c;
	}
	for (int p = 0; p < 2; p++)
		if (rel[0] * kN[p][0] + rel[1] * kN[p][1] + rel[2] * kN[p][2] - kD >= 0.0f)
			return false;
	return true;
}

// Shared tail of the two car paths.
static void drawCarWith(const Model3D* m, const C3D_Mtx* mvp, const C3D_Mtx* normalMtx,
		float lx, float ly, float lz) {
	bindProg(&s_carShader.program);
	C3D_SetAttrInfo((C3D_AttrInfo*)&m->attr);
	C3D_SetBufInfo((C3D_BufInfo*)&m->buf);

	wheelBegin(m, mvp, normalMtx, NULL, NULL, false);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_carShader.uLightDir, lx, ly, lz, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_carShader.uShade, CAR_AMBIENT, CAR_DIFFUSE, 0.0f, 0.0f);
	drawCarBatches(m);
}

// The SH car (#166): carsh.v.pica with the car's light context, and every
// batch's own material and gloss.
static void drawCarSh(const Model3D* m, const C3D_Mtx* mvp, const float rot[3][3],
		const float origin[3]) {
	bindProg(&s_shShader.program);
	C3D_SetAttrInfo((C3D_AttrInfo*)&m->attr);
	C3D_SetBufInfo((C3D_BufInfo*)&m->buf);

	wheelBegin(m, mvp, NULL, rot, origin, true);
	C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_shShader.uGloss, false);

	s_shBoundMat = -1;
	s_shDraw = true;
	drawCarBatches(m);
	s_shDraw = false;
}

// Callers speak 0xRRGGBB, which is what a PAINT.nfsp entry's RED/GREEN/BLUE
// make; TEV constants are 0xAABBGGRR.
static u32 tevColor(u32 rgb) {
	return 0xFF000000u | ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu);
}

void model3d_set_car_paint(u32 rgb) {
	s_carPaint = tevColor(rgb);
}

void model3d_reset_car_paint(void) {
	s_carPaint = CAR_PAINT_COLOR;
	model3d_set_car_slot_paints(NULL, NULL);
}

void model3d_set_car_slot_paints(const u32* rgb, const bool* set) {
	s_carSlotPaintAny = false;
	for (int i = 0; i < MODEL3D_CAR_PAINT_SLOTS; i++) {
		// Slot 0 is the body, which s_carPaint already is.
		const bool on = rgb && set && i > 0 && set[i];
		s_carSlotPaint[i] = on ? tevColor(rgb[i]) : 0;
		s_carSlotPaintAny |= on;
	}
}

void model3d_set_model_paints(Model3D* m, u32 body, const u32* rgb, const bool* set) {
	if (!m)
		return;
	m->ownPaint = true;
	m->ownPaintBody = body > 0xFFFFFFu ? CAR_PAINT_COLOR : tevColor(body);
	m->ownSlotPaintAny = false;
	for (int i = 0; i < MODEL3D_CAR_PAINT_SLOTS; i++) {
		const bool on = rgb && set && i > 0 && set[i];
		m->ownSlotPaint[i] = on ? tevColor(rgb[i]) : 0;
		m->ownSlotPaintAny |= on;
	}
}

void model3d_draw_car(const Model3D* m, const C3D_Mtx* vp, const float pos[3],
	float yaw, float pitch, float roll)
{
	if (!m || m->shading != SHADING_LIT || !s_carShader.ready)
		return;

	// Model space out of GEOMETRY.BIN is +X front, +Y left, +Z up, centred
	// between the axles -- the same space the room is in, at the same scale, so
	// the car needs no normalisation to stand on a disc.
	//
	// The origin is NOT on the wheel-contact plane, though: it sits a little
	// above it, by exactly the model's own -bbmin[2] (231 mm on the 240SX, with
	// 32 verts -- four wheels x eight -- sitting on that plane). Translating
	// straight to `pos` would therefore bury that much tire in the disc, so the
	// lift goes in here and `pos` gets to mean what it claims to.
	//
	// Mtx_* with bRightSide=true post-multiplies, so these read in reverse
	// order of application: lift the model onto its own contact plane, roll,
	// pitch, yaw, then place. Doing the lift FIRST is what makes the three
	// rotations pivot about the contact point rather than about the origin
	// hanging above it -- otherwise a pitched car swings its nose down through
	// the road by the height of that offset.
	//
	// Z-up, +Y-left means the signs are not the textbook ones: rotating about
	// +Y takes the nose down, so nose-up positive is -pitch; the right flank is
	// -Y, so right-side-down positive is +roll.
	C3D_Mtx model, normalMtx;
	Mtx_Identity(&model);
	Mtx_Translate(&model, pos[0], pos[1], pos[2], true);
	Mtx_RotateZ(&model, yaw, true);
	Mtx_RotateY(&model, -pitch, true);
	Mtx_RotateX(&model, roll, true);
	Mtx_Translate(&model, 0.0f, 0.0f, -m->bbmin[2], true);

	// World-space normals against a world-space light: the light then stays
	// put in the alley while the car turns under it, so highlights sweep along
	// the flanks instead of being welded to the camera.
	Mtx_Identity(&normalMtx);
	Mtx_RotateZ(&normalMtx, yaw, true);
	Mtx_RotateY(&normalMtx, -pitch, true);
	Mtx_RotateX(&normalMtx, roll, true);

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, (C3D_Mtx*)vp, &model);
	// The frontend's cars are lit as RenderFrontEndCars lights them: by
	// CarRenderInfo::Render, as the world's are, under the platform's rig.
	// The SH path wants the transform as model3d_draw_car_xform takes it --
	// the car's axes as rows and the model origin -- which is the model
	// matrix's columns.
	if (m->lightMat && s_carViewSet && s_shShader.ready && !g_dbgCarPlainLight) {
		float rot[3][3], origin[3];
		for (int i = 0; i < 3; i++) {
			rot[0][i] = model.r[i].x;
			rot[1][i] = model.r[i].y;
			rot[2][i] = model.r[i].z;
			origin[i] = model.r[i].w;
		}
		drawCarSh(m, &mvp, (const float (*)[3])rot, origin);
		return;
	}
	// `lightDir` is the direction *toward* the light (car.v.pica dots it with
	// the world normal and clamps at zero), and world +Z is up, so this has to
	// be Z-positive: the street lamps in Showroom hang above and slightly
	// behind the discs. It was Z-negative, which put the light under the floor
	// and left every up-facing panel -- the whole of the car you can see --
	// clamped to the CAR_AMBIENT floor. Unit length.
	drawCarWith(m, &mvp, &normalMtx, 0.0f, -0.406f, 0.914f);
}

void model3d_draw_car_xform(const Model3D* m, const C3D_Mtx* vp, const float rot[3][3],
	const float origin[3])
{
	if (!m || m->shading != SHADING_LIT || !s_carShader.ready)
		return;
	// The car's passes set TEV stage 0 and leave 1-5 as they find them, which
	// was pass-through only because the env faces, the headlight's beam stages
	// or the reflection's resolve had reset them earlier in the frame. At the
	// lowest Level of Detail none of those run, and the previous frame's 2D
	// pass left a stage and citro2d's procedural texture live: the body came
	// out flat and unlit (TASKS.md #363). Put them back here, as
	// reflection.c's and fe_post.c's passes do for theirs.
	C3D_ProcTexBind(0, NULL);
	for (int i = 1; i < 6; i++) {
		C3D_TexEnv* e = C3D_GetTexEnv(i);
		C3D_TexEnvInit(e);
		C3D_SetTexEnv(i, e);
	}

	// The model's space is the record's (x front, y left, z up, from the
	// record origin), so the body's own orientation and the origin's world
	// position are the whole transform: column j is the car's axis j in world.
	C3D_Mtx model, normalMtx;
	Mtx_Identity(&model);
	for (int i = 0; i < 3; i++) {
		model.r[i].x = rot[0][i];
		model.r[i].y = rot[1][i];
		model.r[i].z = rot[2][i];
		model.r[i].w = origin[i];
	}
	normalMtx = model;
	for (int i = 0; i < 3; i++)
		normalMtx.r[i].w = 0.0f;

	C3D_Mtx mvp;
	Mtx_Multiply(&mvp, (C3D_Mtx*)vp, &model);
	s_carRimSwap = rimSwap(m, rot);
	if (m->lightMat && s_carViewSet && s_shShader.ready && !g_dbgCarPlainLight)
		drawCarSh(m, &mvp, rot, origin);
	else
		drawCarWith(m, &mvp, &normalMtx, 0.0f, -0.406f, 0.914f);
}

void model3d_draw_car_solo(const Model3D* m, float angle, float aspect) {
	if (!m || m->shading != SHADING_LIT || !s_carShader.ready)
		return;

	// As above, but with no room to stand in: scale to unit size, recentre on
	// the bbox, spin, tip Z-up into the view's Y-up (that is what the -pi/2
	// is) while tilting the camera down over the roof, then push the whole
	// thing away from the eye.
	C3D_Mtx model, normalMtx, proj, mvp;
	float s = m->scale;
	Mtx_Identity(&model);
	Mtx_Translate(&model, 0.0f, 0.0f, -CAR_VIEW_DIST, true);
	Mtx_RotateX(&model, -M_PI / 2.0f + CAR_VIEW_TILT, true);
	Mtx_RotateZ(&model, angle, true);
	Mtx_Translate(&model, -m->center[0] * s, -m->center[1] * s, -m->center[2] * s, true);
	Mtx_Scale(&model, s, s, s);

	// Normals get the rotation only. No scale is folded in, so they stay unit
	// length and the shader does not have to renormalise.
	Mtx_Identity(&normalMtx);
	Mtx_RotateX(&normalMtx, -M_PI / 2.0f + CAR_VIEW_TILT, true);
	Mtx_RotateZ(&normalMtx, angle, true);

	Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(CAR_VIEW_FOV), aspect, 0.1f, 50.0f, false);
	Mtx_Multiply(&mvp, &proj, &model);

	// Key light over the viewer's right shoulder, in view space, unit length.
	drawCarWith(m, &mvp, &normalMtx, 0.399f, 0.699f, 0.594f);
}

void model3d_set_untextured_paint(Model3D* m, u32 rgba)
{
	if (m)
		m->untexPaint = rgba;
}

// The PICA200's order inside an 8x8 tile: x in the even bits, y in the odd
// (tools/car_pack.py _TILE).
static u8 mortonIndex(u32 x, u32 y) {
	return (u8)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
	            ((x & 4) << 2) | ((y & 4) << 3));
}

// `argb` box-halved in place, 2x2 to 1 with each byte the truncated mean --
// tools/car_pack.py's box_downscale(), which made every other car mip.
static void halveArgb(u32* argb, u32 w, u32 h) {
	const u32 nw = w / 2, nh = h / 2;
	for (u32 y = 0; y < nh; y++)
		for (u32 x = 0; x < nw; x++) {
			const u32* s = argb + (y * 2) * w + x * 2;
			const u32 p[4] = { s[0], s[1], s[w], s[w + 1] };
			u32 out = 0;
			for (int sh = 0; sh < 32; sh += 8) {
				u32 acc = 0;
				for (int k = 0; k < 4; k++)
					acc += (p[k] >> sh) & 0xFF;
				out |= (acc / 4) << sh;
			}
			// Every source texel this reads is at or past the one written.
			argb[y * nw + x] = out;
		}
}

// Level `lv` of `t` from `argb` (cw x ch, row 0 the top), in t's own format.
// Tiles run row-major from the image's top row, like every packed texture
// (the kit's V was negated to match). GPU_RGBA8 is A, B, G, R in memory, which
// is 0xAARRGGBB rotated left a byte; RGB565 and RGBA4 keep each channel's top
// bits, tools/car_pack.py's encode_texture(). False for any other format.
static bool writeLevel(C3D_Tex* t, int lv, const u32* argb, u32 cw, u32 ch) {
	static u8 tile[64];
	if (!tile[1])
		for (u32 i = 0; i < 64; i++)
			tile[i] = mortonIndex(i & 7, i >> 3);
	const GPU_TEXCOLOR fmt = t->fmt;
	if (fmt != GPU_RGBA8 && fmt != GPU_RGB565 && fmt != GPU_RGBA4)
		return false;
	void* dst = C3D_Tex2DGetImagePtr(t, lv, NULL);
	const u32 tilesX = cw / 8;
	for (u32 y = 0; y < ch; y++) {
		const u32 rowAt = ((y >> 3) * tilesX) * 64;
		const u32* src = argb + y * cw;
		const u32 iy = (y & 7) << 3;
		for (u32 x = 0; x < cw; x++) {
			const u32 px = src[x];
			const u32 at = rowAt + (x >> 3) * 64 + tile[iy | (x & 7)];
			if (fmt == GPU_RGBA8) {
				((u32*)dst)[at] = (px << 8) | (px >> 24);
			} else {
				const u32 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
				((u16*)dst)[at] = fmt == GPU_RGB565
					? (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
					: (u16)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (px >> 28));
			}
		}
	}
	return true;
}

void model3d_tex_alloc_fails(u32* count, u32* kb)
{
	if (count) *count = s_texAllocFails;
	if (kb)    *kb = s_texAllocFailKB;
}

bool model3d_set_skin(Model3D* m, u32* argb, int w, int h)
{
	if (!m || !m->texSkin || !argb || w < 8 || h < 8)
		return false;
	int first = -1;
	for (int i = 0; i < m->numTextures; i++)
		if (m->texSkin[i] && m->texValid[i]) {
			first = i;
			break;
		}
	if (first < 0)
		return false;
	u32 cw = (u32)w, ch = (u32)h;
	const u32 tw = m->textures[first].width, th = m->textures[first].height;
	while (cw > tw && ch > th) {
		halveArgb(argb, cw, ch);
		cw /= 2;
		ch /= 2;
	}
	if (cw != tw || ch != th)
		return false;
	const int levels = m->textures[first].maxLevel;
	for (int lv = 0; lv <= levels; lv++) {
		if (lv) {
			halveArgb(argb, cw, ch);
			cw /= 2;
			ch /= 2;
		}
		for (int i = first; i < m->numTextures; i++) {
			C3D_Tex* t = &m->textures[i];
			if (!m->texSkin[i] || !m->texValid[i] || t->width != tw || t->height != th ||
			    t->maxLevel < lv)
				continue;
			writeLevel(t, lv, argb, cw, ch);
		}
	}
	for (int i = first; i < m->numTextures; i++)
		if (m->texSkin[i] && m->texValid[i])
			C3D_TexFlush(&m->textures[i]);
	return true;
}

bool model3d_set_texture(Model3D* m, u32 hash, u32* argb, int w, int h)
{
	if (!m || !m->texHashes || m->extTextures || !argb || w < 8 || h < 8)
		return false;
	int slot = -1;
	for (int i = 0; i < m->numTextures && slot < 0; i++)
		if (m->texHashes[i] == hash && m->texValid[i] && !m->texSkin[i])
			slot = i;
	if (slot < 0)
		return false;
	C3D_Tex* t = &m->textures[slot];
	u32 cw = (u32)w, ch = (u32)h;
	while (cw > t->width && ch > t->height) {
		halveArgb(argb, cw, ch);
		cw /= 2;
		ch /= 2;
	}
	if (cw != t->width || ch != t->height)
		return false;
	// #436: never written while shared -- a private texture of the same shape
	// and state takes the borrowed one's place first. Only once the write is
	// sure to go through (size matched above, a format writeLevel takes),
	// since every level is written below and nothing is copied over.
	if (m->texShare && m->texShare[slot] >= 0) {
		const C3D_Tex was = *t;
		if (was.fmt != GPU_RGBA8 && was.fmt != GPU_RGB565 && was.fmt != GPU_RGBA4)
			return false;
		C3D_TexInitParams tp = { was.width, was.height, was.maxLevel, was.fmt, GPU_TEX_2D, false };
		C3D_Tex own;
		if (!C3D_TexInitWithParams(&own, NULL, tp))
			return false;
		own.param = was.param;
		own.border = was.border;
		own.lodParam = was.lodParam;
		shareRelease(m->texShare[slot]);
		m->texShare[slot] = -1;
		*t = own;
	}
	for (int lv = 0; lv <= t->maxLevel; lv++) {
		if (lv) {
			halveArgb(argb, cw, ch);
			cw /= 2;
			ch /= 2;
		}
		if (!writeLevel(t, lv, argb, cw, ch))
			return false;
	}
	C3D_TexFlush(t);
	return true;
}

void model3d_set_scenery(Model3D* m, const u8* setOn)
{
	if (m)
		m->sceneryOn = setOn;
}

// ------------------------------------------------------------ ground probe
//
// A straight-down ray test against the section's own triangles. There is no
// separate collision mesh in the retail data for this to use -- the streamed
// sections carry the drawn geometry and nothing else -- so the drawn road *is*
// the collision surface, which is what NFSU2's own precomputed surface data is
// baked from anyway.
//
// Only near-horizontal triangles are considered: a facade the car is driving
// past passes the 2D containment test just as happily as the tarmac under it
// does, and letting a wall answer "the ground is here" would teleport the car
// up the side of the building. `WALK_NZ` is the cosine of the steepest slope
// treated as ground -- 0.35 is about 70 degrees, loose enough for the game's
// steepest ramps and parking-garage helices.
//
// The face also has to point UP. `det` is twice the SIGNED projected area, so
// its sign is the sign of the face normal's z, and testing `det * det` alone
// discards it -- which let the underside of anything horizontal answer as
// ground: a canopy soffit, a bridge deck seen from below, a shop awning. That
// is not a corner case in this city. Over 40 sections, 44.5% of the faces
// passing the slope test alone face DOWN, and their median centre is z 49.4
// against 38.7 for the ones facing up -- they are the ceilings above the
// streets, and the probe was happily standing the car on them.
//
// It cost two separate bugs. At the career spawn the car was parked on the
// underside of an overpass and fell to the street (#178, worked around there
// by tightening that one ceiling). And while driving, one wheel of four would
// latch onto a soffit 2.3 m up while the other three sat on flat tarmac, which
// sampleGround() turned into a 24-degree pitch and carsim's slope term turned
// into 4 m/s^2 of acceleration -- the car sliding off on its own with no input
// on visibly level ground (#181). Requiring det > 0 fixes both at the source.
//
// Checked before changing it: over 1,464 sampled points that have ground
// today, 86.1% get the same answer, 12.6% correctly drop to the real surface
// (median 9.45 m below the underside they had been standing on), and 1.4% have
// no upward face at all -- inside buildings and under roofs, where there is no
// floor in the data and the honest answer is "no ground", which carsim already
// handles by holding height rather than falling.
//
// model3d_walls_near() keeps the sign-blind form on purpose: a wall is a wall
// from either side.
//
// The triangles come from the physics grid below when the model has one, and
// from the whole pool when it does not. The pool walk was the original, on
// the reading that a street section is 8-25k indices; C4 is 91k, and the
// walk is what cost the world its frame rate (see the grid's own comment).
#define WALK_NZ 0.35f

void model3d_phys_span(const Model3D* m, int* outPhys, int* outTotal)
{
	if (outPhys)  *outPhys  = m ? m->physIndices : 0;
	if (outTotal) *outTotal = m ? m->numIndices  : 0;
}

// ------------------------------------------------------------ physics grid
//
// ------------------------------------------------- building the cull leaves
//
// TASKS.md #68. The GameCube build culls scenery per *instance*: a section
// carries a tree of 0x24-byte nodes, ScenerySectionHeader::TreeCull
// (0x80209724) walks it testing each node's box with GetVisibleStateSB, and a
// leaf is one scenery instance handed to DrawAScenery (0x80208fa4), which
// tests that instance's own centre/extent box before drawing it.
//
// This port cannot have leaves that fine. tools/track_pack.py merges a
// section's subsets by (texture, flags) because a section drawn per instance is
// "over a thousand draw calls" and four resident sections overrun the GPU
// command buffer -- but the merge is exactly what destroys per-instance bounds:
// the road texture's batch spans the whole section, so its box is the section's
// box and a test against it can never reject anything. Which is why the port
// had nothing finer than world.c's per-section test, and why the section the
// camera stands in submitted all of itself when the frustum covers 82 degrees
// of the 360 around it.
//
// So: retail's tree with coarser leaves and one level instead of many. Each
// merged batch's triangles are sorted into a grid of XY squares, each square
// becomes a contiguous run of that batch's indices with its own box, and the
// three states carry over unchanged -- a batch outside costs one test, one
// fully inside is drawn whole with no cell tests at all (TreeCull's inherited
// state 2), and only a straddling batch walks its cells. Neighbouring kept
// cells are contiguous by construction, so a run of them coalesces back into
// one draw call: the split costs draw calls only where it is actually cutting
// geometry away, and the batch test above it removes more than the cells add.
//
// Measured over the shipped L4RA (tools/world_cullstudy.py, 320 camera samples
// at 40 spots x 8 yaws, reproducing world.c's own draw rules):
//
//                      triangles   draw calls   box tests   table
//   as drawn before       80201         575            -        -
//   32 m cells            37388         430         2468   7.0 KB
//   48 m cells            38344         416         2038   5.3 KB
//   64 m cells            39341         407         1802   4.3 KB
//   96 m cells            41629         397         1562   3.4 KB
//
// 48 m: -52.2% triangles and -27.8% draw calls, for about 5 KB a section and
// two thousand box tests a frame. Finer squares buy another 1% of triangles
// and cost a third more tests and table, so this is where it settles.
//
// XY only, like the physics grid and like world.c's own distance test: the
// camera sits near the ground looking roughly along it, so the frustum's
// vertical spread covers nearly everything a square holds anyway.
#define CULL_CELL      48.0f   // m, the grid square (see the table above)
#define CULL_MAX_SIDE  16      // squares across a batch; past it the square grows
#define CULL_MIN_TRIS  192     // below this a batch stays one cell

// Keys fit a byte only while this holds, and the histogram is on the stack.
_Static_assert(CULL_MAX_SIDE * CULL_MAX_SIDE <= 256, "cull key must fit a u8");

static inline int cullCellOf(float v, float origin, float inv)
{
	const int c = (int)((v - origin) * inv);
	return c < 0 ? 0 : (c >= CULL_MAX_SIDE ? CULL_MAX_SIDE - 1 : c);
}

// ------------------------------------------------- the instance table (#247)
//
// A world section packed since #247 carries the instances its scenery was
// placed from and, for every batch, the runs of its indices each instance
// draws (track_pack.py's pack_instance_block). Those replace the grid cells as
// the leaves of the cull -- an instance's own box is retail's leaf, the one
// DrawAScenery asks GetVisibleState about -- and they are what lets each
// instance take its own level of detail (see instancePick).
//
// The one thing kept from the cells is the cut of a large run. A road or a
// terrain piece is one instance whose box is the section's, which no test can
// reject, so a run of CULL_MIN_TRIS or more is sorted into squares exactly as a
// batch is above, each piece with a box of its own. That is a finer test than
// retail makes and never a coarser one: it changes only what is drawn off
// screen.

// The box over the vertices `count` indices from `first` touch. False for a
// NaN in the pool.
static bool indexBox(const Model3D* m, u32 first, u32 count, float lo[3],
		float hi[3])
{
	const u8* vp = (const u8*)m->verts;
	const u16* ix = m->indices + first;
	const float* p = (const float*)(vp + (u32)ix[0] * VERTEX_SIZE);
	for (int k = 0; k < 3; k++)
		lo[k] = hi[k] = p[k];
	for (u32 i = 1; i < count; i++) {
		p = (const float*)(vp + (u32)ix[i] * VERTEX_SIZE);
		for (int k = 0; k < 3; k++) {
			if (p[k] < lo[k]) lo[k] = p[k];
			if (p[k] > hi[k]) hi[k] = p[k];
		}
	}
	for (int k = 0; k < 3; k++) {
		if (!(hi[k] >= lo[k]))
			return false;
	}
	return true;
}

static inline void fileRun(const u8* rp, u32 i, u32* first, u32* tris, u32* inst)
{
	u32 f;
	u16 t, n;
	memcpy(&f, rp + i * INST_RUN_SIZE, 4);
	memcpy(&t, rp + i * INST_RUN_SIZE + 4, 2);
	memcpy(&n, rp + i * INST_RUN_SIZE + 6, 2);
	*first = f;
	*tris = t;
	*inst = n;
}

typedef struct {
	ModelRun* runs;
	u32 n, cap;
	RunBox* boxes;
	u32 nb, bcap;
} RunBuild;

static bool pushRun(RunBuild* rb, u32 first, u32 tris, u32 inst, u32 box)
{
	if (rb->n == rb->cap) {
		const u32 want = rb->cap ? rb->cap * 2 : 512;
		ModelRun* grown = (ModelRun*)realloc(rb->runs, (size_t)want * sizeof(ModelRun));
		if (!grown)
			return false;
		rb->runs = grown;
		rb->cap = want;
	}
	ModelRun* r = &rb->runs[rb->n++];
	r->first = first;
	r->tris = (u16)tris;
	r->inst = (u16)inst;
	r->box = box;
	return true;
}

static bool pushBox(RunBuild* rb, const float lo[3], const float hi[3])
{
	if (rb->nb == rb->bcap) {
		const u32 want = rb->bcap ? rb->bcap * 2 : 128;
		RunBox* grown = (RunBox*)realloc(rb->boxes, (size_t)want * sizeof(RunBox));
		if (!grown)
			return false;
		rb->boxes = grown;
		rb->bcap = want;
	}
	memcpy(rb->boxes[rb->nb].lo, lo, sizeof(float) * 3);
	memcpy(rb->boxes[rb->nb].hi, hi, sizeof(float) * 3);
	rb->nb++;
	return true;
}

// One large run of one instance, cut into boxed pieces; `tmp` and `key` hold
// at least its indices and its triangles.
static bool splitRun(const Model3D* m, RunBuild* rb, u32 first, u32 tris,
		u32 inst, u16* tmp, u8* key)
{
	float lo[3], hi[3];
	if (!indexBox(m, first, tris * 3, lo, hi))
		return pushRun(rb, first, tris, inst, RUN_NO_BOX);
	float step = CULL_CELL;
	for (int k = 0; k < 2; k++) {
		const float want = (hi[k] - lo[k]) / (float)CULL_MAX_SIDE;
		if (want > step)
			step = want;
	}
	const float inv = 1.0f / step;

	const u8* vp = (const u8*)m->verts;
	u16* ix = m->indices + first;
	u32 hist[CULL_MAX_SIDE * CULL_MAX_SIDE];
	u32 off[CULL_MAX_SIDE * CULL_MAX_SIDE];
	memset(hist, 0, sizeof(hist));
	for (u32 t = 0; t < tris; t++) {
		const float* a = (const float*)(vp + (u32)ix[t * 3]     * VERTEX_SIZE);
		const float* b = (const float*)(vp + (u32)ix[t * 3 + 1] * VERTEX_SIZE);
		const float* c = (const float*)(vp + (u32)ix[t * 3 + 2] * VERTEX_SIZE);
		const int gx = cullCellOf((a[0] + b[0] + c[0]) * (1.0f / 3.0f), lo[0], inv);
		const int gy = cullCellOf((a[1] + b[1] + c[1]) * (1.0f / 3.0f), lo[1], inv);
		const u32 k = (u32)gx * CULL_MAX_SIDE + (u32)gy;
		key[t] = (u8)k;
		hist[k]++;
	}
	u32 used = 0, run = 0;
	for (u32 k = 0; k < CULL_MAX_SIDE * CULL_MAX_SIDE; k++) {
		off[k] = run;
		run += hist[k];
		if (hist[k])
			used++;
	}
	// All in one square: the instance's box already says everything a box of
	// the piece would.
	if (used < 2)
		return pushRun(rb, first, tris, inst, RUN_NO_BOX);
	for (u32 t = 0; t < tris; t++) {
		const u32 d = off[key[t]]++;
		tmp[d * 3]     = ix[t * 3];
		tmp[d * 3 + 1] = ix[t * 3 + 1];
		tmp[d * 3 + 2] = ix[t * 3 + 2];
	}
	memcpy(ix, tmp, (size_t)tris * 3 * 2);

	u32 start = 0;
	for (u32 k = 0; k < CULL_MAX_SIDE * CULL_MAX_SIDE; k++) {
		const u32 c = hist[k];
		if (!c)
			continue;
		float clo[3], chi[3];
		indexBox(m, first + start * 3, c * 3, clo, chi);
		if (!pushBox(rb, clo, chi) ||
		    !pushRun(rb, first + start * 3, c, inst, rb->nb - 1))
			return false;
		start += c;
	}
	return true;
}

// Parses m->instBlock into the instance, run and box tables. Returns their
// size in bytes, or 0 -- with nothing changed but, possibly, the order of
// triangles inside a run -- if the block does not describe this model.
static size_t buildInstanced(Model3D* m)
{
	const u8* blk = m->instBlock;
	const u32 bytes = m->instBlockBytes;
	if (!blk || bytes < INST_BLOCK_HDR)
		return 0;
	u32 magic, nInst, nRun, envSplit;
	memcpy(&magic, blk, 4);
	memcpy(&nInst, blk + 4, 4);
	memcpy(&nRun, blk + 8, 4);
	memcpy(&envSplit, blk + 12, 4);
	// The env tier's start (#166), taken even if the rest of the table is
	// refused, so that the far draw of a model left without one still stops
	// short of it. A start that does not sit past the far run's is ignored.
	if (magic == INST_BLOCK_MAGIC && envSplit &&
	    (int)envSplit - 1 >= m->lodFarFirst && (int)envSplit - 1 <= m->numBatches)
		m->envFirst = (int)envSplit - 1;
	if (magic != INST_BLOCK_MAGIC || !nInst || nInst > 0xFFFFu || !nRun ||
	    (u64)INST_BLOCK_HDR + (u64)nInst * INST_RECORD_SIZE +
	    (u64)nRun * INST_RUN_SIZE > bytes)
		return 0;
	const u8* rp = blk + INST_BLOCK_HDR + nInst * INST_RECORD_SIZE;

	// The runs in pool order, none overlapping, each inside the pool and
	// naming a real instance.
	u32 prevEnd = 0, maxSplit = 0;
	for (u32 i = 0; i < nRun; i++) {
		u32 first, tris, inst;
		fileRun(rp, i, &first, &tris, &inst);
		if (!tris || inst >= nInst || first < prevEnd ||
		    (u64)first + (u64)tris * 3 > (u64)m->numIndices)
			return 0;
		prevEnd = first + tris * 3;
		if (tris >= CULL_MIN_TRIS && tris > maxSplit)
			maxSplit = tris;
	}
	// Every batch tiled exactly by consecutive runs, found by its first index.
	// Since the runs cannot overlap, neither can two batches that pass.
	for (int bi = 0; bi < m->numBatches; bi++) {
		ModelBatch* bt = &m->batches[bi];
		bt->runFirst = 0;
		bt->runCount = 0;
		if (!bt->indexCount)
			continue;
		u32 lo = 0, hi = nRun;
		while (lo < hi) {
			const u32 mid = (lo + hi) / 2;
			u32 f, t, n;
			fileRun(rp, mid, &f, &t, &n);
			if (f < bt->firstIndex)
				lo = mid + 1;
			else
				hi = mid;
		}
		u32 sum = 0, j = lo;
		while (j < nRun && sum < bt->indexCount) {
			u32 f, t, n;
			fileRun(rp, j, &f, &t, &n);
			if (f != bt->firstIndex + sum)
				break;
			sum += t * 3;
			j++;
		}
		if (sum != bt->indexCount)
			goto refuse;
		bt->runFirst = lo;       // the file's numbering for now
		bt->runCount = j - lo;
	}

	ModelInstance* insts = (ModelInstance*)malloc((size_t)nInst * sizeof(ModelInstance));
	u8* state = (u8*)calloc(nInst, 1);
	u16* tmp = maxSplit ? (u16*)malloc((size_t)maxSplit * 3 * 2) : NULL;
	u8* key = maxSplit ? (u8*)malloc(maxSplit) : NULL;
	RunBuild rb = { 0 };
	if (!insts || !state || (maxSplit && (!tmp || !key)))
		goto bail;
	memcpy(insts, blk + INST_BLOCK_HDR, (size_t)nInst * sizeof(ModelInstance));

	for (int bi = 0; bi < m->numBatches; bi++) {
		ModelBatch* bt = &m->batches[bi];
		const u32 src = bt->runFirst, srcCount = bt->runCount;
		bt->runFirst = rb.n;
		bt->runCount = 0;
		if (!srcCount)
			continue;
		// The batch's own box, the first test the draw loop makes. A NaN in
		// the pool makes it a box nothing can be outside of, so the instances
		// still answer for themselves.
		if (!indexBox(m, bt->firstIndex, bt->indexCount, bt->lo, bt->hi)) {
			for (int k = 0; k < 3; k++) {
				bt->lo[k] = -1e30f;
				bt->hi[k] = 1e30f;
			}
		}
		for (u32 j = src; j < src + srcCount; j++) {
			u32 first, tris, inst;
			fileRun(rp, j, &first, &tris, &inst);
			const bool ok = tris >= CULL_MIN_TRIS
				? splitRun(m, &rb, first, tris, inst, tmp, key)
				: pushRun(&rb, first, tris, inst, RUN_NO_BOX);
			if (!ok)
				goto bail;
		}
		bt->runCount = rb.n - bt->runFirst;
	}
	free(tmp);
	free(key);

	// Only a model with something in a reflection gets the env view's state.
	// Without the memory it is simply left out of the env faces.
	u8* envState = NULL;
	for (u32 i = 0; i < nInst; i++) {
		if (insts[i].env & INST_ENV_KIND) {
			envState = (u8*)calloc(nInst, 2);
			break;
		}
	}
	m->envMaxR = 0.0f;
	for (int k = 0; k < 3; k++) {
		m->envLo[k] = 1e30f;
		m->envHi[k] = -1e30f;
	}
	for (u32 i = 0; envState && i < nInst; i++) {
		const ModelInstance* in = &insts[i];
		if (!(in->env & INST_ENV_KIND) || in->radius < 0.0f)
			continue;
		for (int k = 0; k < 3; k++) {
			if (in->pos[k] < m->envLo[k]) m->envLo[k] = in->pos[k];
			if (in->pos[k] > m->envHi[k]) m->envHi[k] = in->pos[k];
		}
		if (in->radius > m->envMaxR)
			m->envMaxR = in->radius;
	}

	// The reflection views' mirror rows (#296), after the runs. A row number
	// past the table's end, or no table, leaves the model out of them.
	{
		const u32 at = INST_BLOCK_HDR + nInst * INST_RECORD_SIZE + nRun * INST_RUN_SIZE;
		u32 rmagic = 0, nRows = 0;
		if (at + 8 <= bytes) {
			memcpy(&rmagic, blk + at, 4);
			memcpy(&nRows, blk + at + 4, 4);
		}
		if (rmagic == REFL_TABLE_MAGIC && (u64)at + 8 + (u64)nRows * 16 <= bytes) {
			bool ok = true;
			for (u32 i = 0; ok && i < nInst; i++)
				ok = (insts[i].env >> INST_ENV_ROW_SHIFT) <= nRows;
			float* rows = nRows ? (float*)malloc((size_t)nRows * 16) : NULL;
			if (ok && (rows || !nRows)) {
				if (nRows)
					memcpy(rows, blk + at + 8, (size_t)nRows * 16);
				m->reflRows = rows;
				m->numReflRows = nRows;
				m->reflTable = envState != NULL;
			} else {
				free(rows);
			}
		}
	}

	m->insts = insts;
	m->numInsts = nInst;
	m->instState = state;
	m->envState = envState;
	m->envFrame = 0;
	m->envAny = 0;
	m->envDistGen = 0;
	m->envDistAny = 0;
	m->instFrame = 0;
	m->instAny = 0;
	m->runs = rb.runs;
	m->numRuns = rb.n;
	m->runBoxes = rb.boxes;
	m->numRunBoxes = rb.nb;
	return (size_t)nInst * (sizeof(ModelInstance) + (envState ? 3 : 1)) +
	       (size_t)rb.n * sizeof(ModelRun) + (size_t)rb.nb * sizeof(RunBox) +
	       (size_t)m->numReflRows * 16;

bail:
	free(insts);
	free(state);
	free(tmp);
	free(key);
	free(rb.runs);
	free(rb.boxes);
refuse:
	for (int bi = 0; bi < m->numBatches; bi++) {
		m->batches[bi].runFirst = 0;
		m->batches[bi].runCount = 0;
	}
	return 0;
}

size_t model3d_build_cull_cells(Model3D* m)
{
	if (!m || m->cells || m->runs || !m->verts || !m->indices ||
	    m->numBatches <= 0)
		return 0;
	// This reorders the index pool, and the physics grid lists triangles by
	// their number in it. Build order is the contract -- world.c's loader does
	// the cells first -- and this is where it is enforced rather than assumed.
	if (m->physGrid)
		return 0;

	// A section with an instance table is culled by its instances instead,
	// and falls back to the grid below only if the table is not usable.
	if (m->instBlock) {
		const size_t built = buildInstanced(m);
		free(m->instBlock);
		m->instBlock = NULL;
		m->instBlockBytes = 0;
		if (built)
			return built;
	}

	u32 maxCount = 0;
	for (int b = 0; b < m->numBatches; b++) {
		if (m->batches[b].indexCount > maxCount)
			maxCount = m->batches[b].indexCount;
	}
	if (maxCount < 3)
		return 0;

	const u8* vp = (const u8*)m->verts;
	u16* tmp = (u16*)malloc((size_t)maxCount * 2);
	u8*  key = (u8*)malloc((size_t)maxCount / 3 + 1);
	if (!tmp || !key) {
		free(tmp);
		free(key);
		return 0;
	}

	CullCell* cells = NULL;
	u32 n = 0, cap = 0;
	float step = CULL_CELL;

	for (int bi = 0; bi < m->numBatches; bi++) {
		ModelBatch* bt = &m->batches[bi];
		bt->cellFirst = n;
		bt->cellCount = 0;
		const u32 nt = bt->indexCount / 3;
		if (!nt || bt->firstIndex + nt * 3 > (u32)m->numIndices)
			continue;
		u16* ix = m->indices + bt->firstIndex;

		// The batch's own box, which is also the first thing the draw loop
		// tests. One walk of its indices, so a degenerate pool shows up here
		// rather than as a cell that never rejects.
		float lo[3], hi[3];
		{
			const float* p = (const float*)(vp + (u32)ix[0] * VERTEX_SIZE);
			for (int k = 0; k < 3; k++)
				lo[k] = hi[k] = p[k];
			for (u32 i = 1; i < nt * 3; i++) {
				p = (const float*)(vp + (u32)ix[i] * VERTEX_SIZE);
				for (int k = 0; k < 3; k++) {
					if (p[k] < lo[k]) lo[k] = p[k];
					if (p[k] > hi[k]) hi[k] = p[k];
				}
			}
		}
		bool sane = true;
		for (int k = 0; k < 3; k++) {
			if (!(hi[k] >= lo[k]))
				sane = false;        // a NaN in the pool
			bt->lo[k] = lo[k];
			bt->hi[k] = hi[k];
		}
		if (!sane)
			continue;                // left with no cells: always drawn whole

		if (n == cap) {
			const u32 want = cap ? cap * 2 : 256;
			CullCell* grown = (CullCell*)realloc(cells, (size_t)want * sizeof(CullCell));
			if (!grown)
				goto bail;
			cells = grown;
			cap = want;
		}

		// Small batches are one cell. Splitting them saves a handful of
		// triangles for a table entry each, and it is what keeps a district
		// shell -- a few hundred triangles spread over kilometres -- from
		// becoming very nearly one cell per triangle.
		if (nt < CULL_MIN_TRIS) {
			memcpy(cells[n].lo, lo, sizeof(lo));
			memcpy(cells[n].hi, hi, sizeof(hi));
			cells[n].first = bt->firstIndex;
			cells[n].count = nt * 3;
			n++;
			bt->cellCount = 1;
			continue;
		}

		// The square is CULL_CELL unless that would make the batch more than
		// CULL_MAX_SIDE squares across, which bounds both the table and the
		// per-frame test count on the sections that sprawl.
		step = CULL_CELL;
		for (int k = 0; k < 2; k++) {
			const float want = (hi[k] - lo[k]) / (float)CULL_MAX_SIDE;
			if (want > step)
				step = want;
		}
		if (!(step > 0.0f))
			step = CULL_CELL;
		const float inv = 1.0f / step;

		u32 hist[CULL_MAX_SIDE * CULL_MAX_SIDE];
		u32 off[CULL_MAX_SIDE * CULL_MAX_SIDE];
		memset(hist, 0, sizeof(hist));
		for (u32 t = 0; t < nt; t++) {
			const float* a = (const float*)(vp + (u32)ix[t * 3]     * VERTEX_SIZE);
			const float* b = (const float*)(vp + (u32)ix[t * 3 + 1] * VERTEX_SIZE);
			const float* c = (const float*)(vp + (u32)ix[t * 3 + 2] * VERTEX_SIZE);
			// The centroid, so a triangle belongs to exactly one square; the
			// square's box is then grown to hold it whole, which is what keeps
			// the test conservative.
			const int gx = cullCellOf((a[0] + b[0] + c[0]) * (1.0f / 3.0f), lo[0], inv);
			const int gy = cullCellOf((a[1] + b[1] + c[1]) * (1.0f / 3.0f), lo[1], inv);
			const u32 k = (u32)gx * CULL_MAX_SIDE + (u32)gy;
			key[t] = (u8)k;
			hist[k]++;
		}
		u32 run = 0;
		for (u32 k = 0; k < CULL_MAX_SIDE * CULL_MAX_SIDE; k++) {
			off[k] = run;
			run += hist[k];
		}
		for (u32 t = 0; t < nt; t++) {
			const u32 d = off[key[t]]++;
			tmp[d * 3]     = ix[t * 3];
			tmp[d * 3 + 1] = ix[t * 3 + 1];
			tmp[d * 3 + 2] = ix[t * 3 + 2];
		}
		memcpy(ix, tmp, (size_t)nt * 3 * 2);

		u32 start = 0;
		for (u32 k = 0; k < CULL_MAX_SIDE * CULL_MAX_SIDE; k++) {
			const u32 c = hist[k];
			if (!c)
				continue;
			if (n == cap) {
				const u32 want = cap ? cap * 2 : 256;
				CullCell* grown = (CullCell*)realloc(cells, (size_t)want * sizeof(CullCell));
				if (!grown)
					goto bail;
				cells = grown;
				cap = want;
			}
			float clo[3], chi[3];
			const float* p = (const float*)(vp + (u32)ix[start * 3] * VERTEX_SIZE);
			for (int q = 0; q < 3; q++)
				clo[q] = chi[q] = p[q];
			for (u32 i = start * 3 + 1; i < (start + c) * 3; i++) {
				p = (const float*)(vp + (u32)ix[i] * VERTEX_SIZE);
				for (int q = 0; q < 3; q++) {
					if (p[q] < clo[q]) clo[q] = p[q];
					if (p[q] > chi[q]) chi[q] = p[q];
				}
			}
			memcpy(cells[n].lo, clo, sizeof(clo));
			memcpy(cells[n].hi, chi, sizeof(chi));
			cells[n].first = bt->firstIndex + start * 3;
			cells[n].count = c * 3;
			n++;
			bt->cellCount++;
			start += c;
		}
	}

	free(tmp);
	free(key);
	if (!n) {
		free(cells);
		return 0;
	}
	m->cells = cells;
	m->numCells = n;
	m->cullStep = step;
	return (size_t)n * sizeof(CullCell);

bail:
	// Out of memory partway through. The indices already sorted are still the
	// same triangles in a different order, which nothing minds, so the only
	// thing to undo is the half-built table: every batch goes back to being
	// drawn whole.
	free(tmp);
	free(key);
	free(cells);
	for (int b = 0; b < m->numBatches; b++)
		m->batches[b].cellCount = 0;
	return 0;
}

void model3d_cull_info(const Model3D* m, int* outCells, float* outCell)
{
	if (outCells) *outCells = m ? (int)m->numCells : 0;
	if (outCell)  *outCell  = m ? m->cullStep : 0.0f;
}

void model3d_inst_info(const Model3D* m, int* outInsts, int* outRuns,
		int* outBoxes)
{
	if (outInsts) *outInsts = m ? (int)m->numInsts : 0;
	if (outRuns)  *outRuns  = m ? (int)m->numRuns : 0;
	if (outBoxes) *outBoxes = m ? (int)m->numRunBoxes : 0;
}

// The scans below used to walk every triangle under physIndices, and round
// the spawn that was most of the frame: C4 alone is 30,334 triangles, and with
// the car inside three or four sections' boxes the ground probe and the wall
// gather measured 4-7 ms each (main.c's `ground` and `walls` profiler slots),
// which is what held the world at 30 FPS there and let it run at 55 a few
// streets away. Nearly all of it was the XY reject throwing a triangle out,
// so the fix is not to look at those triangles at all.
//
// A uniform grid over the section's XY box: each triangle is listed in every
// cell its XY bounding box touches, and a query walks the cells its own box
// touches. What a scan answers does not change -- the same triangles reach
// the same tests -- only how many it looks at to find them.
//
// A triangle spanning several cells is listed in each, and a query box that
// spans several of them would meet it several times. A const query on a
// shared model cannot stamp triangles as seen, so instead each one is
// answered for by exactly one cell: the one holding the low corner of the
// overlap of its box and the query's, which in cell coordinates is the larger
// of its low cell and the query's low cell, per axis. Both cell ranges contain
// that cell whenever they meet at all, so every triangle a query reaches is
// tested exactly once (physWalkOwns()).
//
// Port-side, like the brute force it replaces. Retail answers these questions
// from the track's TopologyTree (Speed.elf: TrackPolygon, and
// DynamicCollisionBody::CreateTrackPolygonCollisionBodies at 0x801af514),
// which this port has not decoded; until it has, the drawn geometry is the
// collision surface (TASKS.md #3) and this only makes reading it cheap.
#define PHYS_CELL          8.0f    // m, a couple of car lengths
#define PHYS_MAX_CELLS     128     // per axis; past it the cell grows instead
#define PHYS_MAX_CELL      64.0f   // m
#define PHYS_REFS_PER_TRI  4       // mean listings per triangle before the cell grows
#define PHYS_MIN_TRIS      64      // below this, walking the pool is already cheap
#define PHYS_PAD           0.01f   // m, so a box ending on a cell edge still reaches it

// Which cell a coordinate falls in, clamped to the grid. Monotonic in v, which
// is all physWalkOwns() needs: a triangle's cells and a query's cells are both
// computed through here from the same floats.
static inline int physCell(float v, float origin, float inv, int n)
{
	const int c = (int)((v - origin) * inv);
	return c < 0 ? 0 : (c >= n ? n - 1 : c);
}

static inline void physTriBox(const u8* vp, const u16* idx, int t, float* lo, float* hi)
{
	const float* a = (const float*)(vp + (u32)idx[t * 3]     * VERTEX_SIZE);
	const float* b = (const float*)(vp + (u32)idx[t * 3 + 1] * VERTEX_SIZE);
	const float* c = (const float*)(vp + (u32)idx[t * 3 + 2] * VERTEX_SIZE);
	for (int k = 0; k < 2; k++) {
		float l = a[k] < b[k] ? a[k] : b[k]; if (c[k] < l) l = c[k];
		float h = a[k] > b[k] ? a[k] : b[k]; if (c[k] > h) h = c[k];
		lo[k] = l;
		hi[k] = h;
	}
}

size_t model3d_build_phys_grid(Model3D* m)
{
	if (!m || m->physGrid || !m->verts || !m->indices)
		return 0;
	const int nt = m->physIndices / 3;
	if (nt <= 0 || nt > 0xFFFF)
		return 0;
	const u8* vp = (const u8*)m->verts;
	const u16* idx = m->indices;

	// The scenery-set batches (TASKS.md #237) are left out of the grid, so
	// the drawn-geometry scans -- the fallback for a card without WORLD.NFST
	// -- never collide with a race barrier, the way they never did when the
	// packer dropped them. Retail's barriers are topology, not scenery. The
	// grid is what keeps them out, so a model that has them gets one however
	// few triangles it has.
	u8* skip = NULL;
	if (m->sceneryOn) {
		for (int b = 0; b < m->numBatches; b++) {
			const ModelBatch* mb = &m->batches[b];
			if (!BATCH_SCENERY_SET(mb->flags) || mb->firstIndex >= (u32)m->physIndices)
				continue;
			if (!skip && !(skip = (u8*)calloc((size_t)nt, 1)))
				return 0;
			u32 end = mb->firstIndex + mb->indexCount;
			if (end > (u32)m->physIndices)
				end = (u32)m->physIndices;
			for (u32 t = mb->firstIndex / 3; t < end / 3; t++)
				skip[t] = 1;
		}
	}
	if (nt < PHYS_MIN_TRIS && !skip)
		return 0;

	// The physics triangles' own box, not the header's: that one also covers
	// the far run, which nothing here is ever asked about.
	float lo[2], hi[2];
	float x0, y0, x1, y1;
	physTriBox(vp, idx, 0, lo, hi);
	x0 = lo[0]; y0 = lo[1]; x1 = hi[0]; y1 = hi[1];
	for (int t = 1; t < nt; t++) {
		physTriBox(vp, idx, t, lo, hi);
		if (lo[0] < x0) x0 = lo[0];
		if (lo[1] < y0) y0 = lo[1];
		if (hi[0] > x1) x1 = hi[0];
		if (hi[1] > y1) y1 = hi[1];
	}
	const float ex = x1 - x0, ey = y1 - y0;
	if (!(ex >= 0.0f) || !(ey >= 0.0f))
		return 0;   // a NaN somewhere in the pool

	// Start small and grow the cell while the grid is too wide or too many
	// triangles straddle cells. The second matters for the district shells:
	// a skyline is a few thousand triangles tens of metres across each.
	float cell = PHYS_CELL;
	int w, h;
	u32 refs;
	for (;;) {
		const float fw = ex / cell, fh = ey / cell;
		if ((fw >= PHYS_MAX_CELLS || fh >= PHYS_MAX_CELLS) && cell < PHYS_MAX_CELL) {
			cell *= 2.0f;
			continue;
		}
		if (fw >= 1024.0f || fh >= 1024.0f) {
			free(skip);
			return 0;   // a stray vertex kilometres out; not worth gridding
		}
		w = (int)fw + 1;
		h = (int)fh + 1;
		const float inv = 1.0f / cell;
		refs = 0;
		for (int t = 0; t < nt; t++) {
			if (skip && skip[t])
				continue;
			physTriBox(vp, idx, t, lo, hi);
			refs += (u32)(physCell(hi[0], x0, inv, w) - physCell(lo[0], x0, inv, w) + 1) *
			        (u32)(physCell(hi[1], y0, inv, h) - physCell(lo[1], y0, inv, h) + 1);
		}
		if (refs > (u32)nt * PHYS_REFS_PER_TRI && cell < PHYS_MAX_CELL) {
			cell *= 2.0f;
			continue;
		}
		break;
	}

	const u32 cells = (u32)w * (u32)h;
	PhysGrid* g = (PhysGrid*)calloc(1, sizeof(PhysGrid));
	if (g) {
		g->start = (u32*)calloc(cells + 1, sizeof(u32));
		g->tris  = (u16*)malloc((refs ? refs : 1) * sizeof(u16));
	}
	if (!g || !g->start || !g->tris) {
		if (g) {
			free(g->start);
			free(g->tris);
			free(g);
		}
		free(skip);
		return 0;
	}
	g->x0 = x0;
	g->y0 = y0;
	g->cell = cell;
	g->inv = 1.0f / cell;
	g->w = w;
	g->h = h;
	g->refs = refs;

	// Counting sort. Count each cell's listings, turn the counts into running
	// ends, then deal slots from the back of each cell's run while walking the
	// triangles backwards -- which leaves start[c] at the run's beginning and
	// every cell in ascending triangle order, so a walk reads the vertex pool
	// roughly front to back.
	for (int t = 0; t < nt; t++) {
		if (skip && skip[t])
			continue;
		physTriBox(vp, idx, t, lo, hi);
		const int cx0 = physCell(lo[0], x0, g->inv, w), cx1 = physCell(hi[0], x0, g->inv, w);
		const int cy0 = physCell(lo[1], y0, g->inv, h), cy1 = physCell(hi[1], y0, g->inv, h);
		for (int cy = cy0; cy <= cy1; cy++)
			for (int cx = cx0; cx <= cx1; cx++)
				g->start[(u32)cy * (u32)w + (u32)cx]++;
	}
	u32 run = 0;
	for (u32 c = 0; c < cells; c++) {
		run += g->start[c];
		g->start[c] = run;
	}
	g->start[cells] = run;
	for (int t = nt - 1; t >= 0; t--) {
		if (skip && skip[t])
			continue;
		physTriBox(vp, idx, t, lo, hi);
		const int cx0 = physCell(lo[0], x0, g->inv, w), cx1 = physCell(hi[0], x0, g->inv, w);
		const int cy0 = physCell(lo[1], y0, g->inv, h), cy1 = physCell(hi[1], y0, g->inv, h);
		for (int cy = cy0; cy <= cy1; cy++)
			for (int cx = cx0; cx <= cx1; cx++)
				g->tris[--g->start[(u32)cy * (u32)w + (u32)cx]] = (u16)t;
	}

	free(skip);
	m->physGrid = g;
	return sizeof(PhysGrid) + (cells + 1) * sizeof(u32) + refs * sizeof(u16);
}

bool model3d_phys_grid_info(const Model3D* m, int* outW, int* outH, float* outCell,
	int* outRefs)
{
	const PhysGrid* g = m ? m->physGrid : NULL;
	if (outW)    *outW    = g ? g->w : 0;
	if (outH)    *outH    = g ? g->h : 0;
	if (outCell) *outCell = g ? g->cell : 0.0f;
	if (outRefs) *outRefs = g ? (int)g->refs : 0;
	return g != NULL;
}

// The triangles a query box can touch, each handed out once per cell it is
// listed in; physWalkOwns() then says whether this is the visit that counts.
// With no grid it is simply every triangle in the pool, in order, which is
// exactly the loop the scans used to run.
typedef struct {
	const PhysGrid* g;
	int cx0, cx1, cy0, cy1;   // the query's cells
	int cx, cy;               // the one being walked
	u32 k, end;               // its run in g->tris
	int next, count;          // no grid: the whole pool
} PhysWalk;

static inline void physWalkBegin(PhysWalk* pw, const Model3D* m,
	float xLo, float xHi, float yLo, float yHi)
{
	const PhysGrid* g = m->physGrid;
	*pw = (PhysWalk){0};
	pw->g = g;
	pw->count = m->physIndices / 3;
	if (!g)
		return;
	pw->cx0 = physCell(xLo - PHYS_PAD, g->x0, g->inv, g->w);
	pw->cx1 = physCell(xHi + PHYS_PAD, g->x0, g->inv, g->w);
	pw->cy0 = physCell(yLo - PHYS_PAD, g->y0, g->inv, g->h);
	pw->cy1 = physCell(yHi + PHYS_PAD, g->y0, g->inv, g->h);
	pw->cx = pw->cx0;
	pw->cy = pw->cy0;
	const u32 c = (u32)pw->cy * (u32)g->w + (u32)pw->cx;
	pw->k = g->start[c];
	pw->end = g->start[c + 1];
}

// The next triangle number, or -1 when the walk is done.
static inline int physWalkNext(PhysWalk* pw)
{
	const PhysGrid* g = pw->g;
	if (!g)
		return pw->next < pw->count ? pw->next++ : -1;
	while (pw->k == pw->end) {
		if (++pw->cx > pw->cx1) {
			pw->cx = pw->cx0;
			if (++pw->cy > pw->cy1)
				return -1;
		}
		const u32 c = (u32)pw->cy * (u32)g->w + (u32)pw->cx;
		pw->k = g->start[c];
		pw->end = g->start[c + 1];
	}
	return g->tris[pw->k++];
}

// Whether the cell being walked is the one that answers for the triangle
// just handed out, given the low corner of its XY box. See the comment at the
// top of this section for why that is exactly one of its visits.
static inline bool physWalkOwns(const PhysWalk* pw, float txLo, float tyLo)
{
	const PhysGrid* g = pw->g;
	if (!g)
		return true;
	int cx = physCell(txLo, g->x0, g->inv, g->w);
	int cy = physCell(tyLo, g->y0, g->inv, g->h);
	if (cx < pw->cx0) cx = pw->cx0;
	if (cy < pw->cy0) cy = pw->cy0;
	return cx == pw->cx && cy == pw->cy;
}

void model3d_bounds(const Model3D* m, float outMin[3], float outMax[3]) {
	for (int k = 0; k < 3; k++) {
		outMin[k] = m->bbmin[k];
		outMax[k] = m->center[k] * 2.0f - m->bbmin[k];
	}
}

bool model3d_ground_z(const Model3D* m, float x, float y, float zMax, float* outZ)
{
	if (!m || !m->verts || !m->indices)
		return false;

	const u8* vp = (const u8*)m->verts;
	const u16* idx = m->indices;
	// physIndices, not numIndices: the far-LOD run at the end of the pool is a
	// coarser copy of props the near run already has, and a road that is two
	// surfaces deep answers this query twice with two different heights. The
	// walk stops there either way.
	PhysWalk pw;
	physWalkBegin(&pw, m, x, x, y, y);
	bool hit = false;
	float best = 0.0f;

	for (int t; (t = physWalkNext(&pw)) >= 0; ) {
		const int i = t * 3;
		const float* a = (const float*)(vp + (u32)idx[i]     * VERTEX_SIZE);
		const float* b = (const float*)(vp + (u32)idx[i + 1] * VERTEX_SIZE);
		const float* c = (const float*)(vp + (u32)idx[i + 2] * VERTEX_SIZE);

		// Cheap XY reject first: most of a section is nowhere near the car.
		float txLo = a[0] < b[0] ? a[0] : b[0]; if (c[0] < txLo) txLo = c[0];
		if (x < txLo) continue;
		float hi = a[0] > b[0] ? a[0] : b[0]; if (c[0] > hi) hi = c[0];
		if (x > hi) continue;
		float lo = a[1] < b[1] ? a[1] : b[1]; if (c[1] < lo) lo = c[1];
		if (y < lo) continue;
		hi = a[1] > b[1] ? a[1] : b[1]; if (c[1] > hi) hi = c[1];
		if (y > hi) continue;
		if (!physWalkOwns(&pw, txLo, lo))
			continue;

		// Signed area of the triangle projected onto XY; it doubles as the
		// barycentric denominator and as the face's own z normal component,
		// so the wall rejection is free.
		float e1x = b[0] - a[0], e1y = b[1] - a[1];
		float e2x = c[0] - a[0], e2y = c[1] - a[1];
		float det = e1x * e2y - e1y * e2x;
		if (det == 0.0f)
			continue;

		float px = x - a[0], py = y - a[1];
		float u = (px * e2y - py * e2x) / det;
		if (u < 0.0f || u > 1.0f) continue;
		float v = (e1x * py - e1y * px) / det;
		if (v < 0.0f || u + v > 1.0f) continue;

		// Reject near-vertical faces, and faces pointing down: det is twice
		// the SIGNED projected area, so dividing it by the true area gives the
		// face normal's z, sign and all. Comparing squares avoids the two
		// square roots; the separate sign test is what keeps a soffit from
		// answering as ground (see WALK_NZ above).
		if (det <= 0.0f)
			continue;
		float nx = e1y * (c[2] - a[2]) - (b[2] - a[2]) * e2y;
		float ny = (b[2] - a[2]) * e2x - e1x * (c[2] - a[2]);
		float lenSq = nx * nx + ny * ny + det * det;
		if (det * det < WALK_NZ * WALK_NZ * lenSq)
			continue;

		float z = a[2] + u * (b[2] - a[2]) + v * (c[2] - a[2]);
		if (z > zMax)
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

void model3d_ground_z_multi(const Model3D* m, const float* xy, int n,
	float zMax, float* outZ, bool* outHit, float* outN)
{
	if (!m || !m->verts || !m->indices || n <= 0)
		return;
	if (n > MODEL3D_MAX_PROBES)
		n = MODEL3D_MAX_PROBES;

	// The points' own XY box, so a triangle nowhere near any of them is
	// rejected once instead of n times.
	float qxLo = xy[0], qxHi = xy[0], qyLo = xy[1], qyHi = xy[1];
	for (int p = 1; p < n; p++) {
		float px = xy[p * 2], py = xy[p * 2 + 1];
		if (px < qxLo) qxLo = px; else if (px > qxHi) qxHi = px;
		if (py < qyLo) qyLo = py; else if (py > qyHi) qyHi = py;
	}

	const u8* vp = (const u8*)m->verts;
	const u16* idx = m->indices;
	PhysWalk pw;
	physWalkBegin(&pw, m, qxLo, qxHi, qyLo, qyHi);

	for (int t; (t = physWalkNext(&pw)) >= 0; ) {
		const int i = t * 3;
		const float* a = (const float*)(vp + (u32)idx[i]     * VERTEX_SIZE);
		const float* b = (const float*)(vp + (u32)idx[i + 1] * VERTEX_SIZE);
		const float* c = (const float*)(vp + (u32)idx[i + 2] * VERTEX_SIZE);

		float txLo = a[0] < b[0] ? a[0] : b[0]; if (c[0] < txLo) txLo = c[0];
		if (qxHi < txLo) continue;
		float txHi = a[0] > b[0] ? a[0] : b[0]; if (c[0] > txHi) txHi = c[0];
		if (qxLo > txHi) continue;
		float tyLo = a[1] < b[1] ? a[1] : b[1]; if (c[1] < tyLo) tyLo = c[1];
		if (qyHi < tyLo) continue;
		float tyHi = a[1] > b[1] ? a[1] : b[1]; if (c[1] > tyHi) tyHi = c[1];
		if (qyLo > tyHi) continue;
		if (!physWalkOwns(&pw, txLo, tyLo))
			continue;

		float e1x = b[0] - a[0], e1y = b[1] - a[1];
		float e2x = c[0] - a[0], e2y = c[1] - a[1];
		float det = e1x * e2y - e1y * e2x;
		if (det == 0.0f)
			continue;

		// Drivability, exactly as model3d_ground_z() tests it -- and hoisted
		// above the per-point work, because a wall is a wall for all n points.
		// This is the one that mattered most for #181: the four wheels are n
		// points of the same query, so one of them picking up a soffit here is
		// what fabricated the pitch that slid the car away.
		if (det <= 0.0f)
			continue;
		float nx = e1y * (c[2] - a[2]) - (b[2] - a[2]) * e2y;
		float ny = (b[2] - a[2]) * e2x - e1x * (c[2] - a[2]);
		float lenSq = nx * nx + ny * ny + det * det;
		if (det * det < WALK_NZ * WALK_NZ * lenSq)
			continue;

		float inv = 1.0f / det;
		float dzb = b[2] - a[2], dzc = c[2] - a[2];

		for (int p = 0; p < n; p++) {
			float x = xy[p * 2], y = xy[p * 2 + 1];
			if (x < txLo || x > txHi || y < tyLo || y > tyHi)
				continue;
			float px = x - a[0], py = y - a[1];
			float u = (px * e2y - py * e2x) * inv;
			if (u < 0.0f || u > 1.0f) continue;
			float v = (e1x * py - e1y * px) * inv;
			if (v < 0.0f || u + v > 1.0f) continue;
			float z = a[2] + u * dzb + v * dzc;
			if (z > zMax) continue;
			if (!outHit[p] || z > outZ[p]) {
				outHit[p] = true;
				outZ[p] = z;
				// The face's own unit normal, up-facing because det > 0 here:
				// the plane the suspension measures against (TASKS.md #222).
				if (outN) {
					float il = 1.0f / sqrtf(lenSq);
					outN[p * 3]	 = nx * il;
					outN[p * 3 + 1] = ny * il;
					outN[p * 3 + 2] = det * il;
				}
			}
		}
	}
}

// ------------------------------------------------------------ wall probe
//
// The other half of the ground probe (TASKS.md #137). Same triangle pool, same
// WALK_NZ threshold, opposite side of it: a face too steep to drive up is one
// the car has to be stopped by, so every triangle in a section is either a
// surface or a wall and the two tests can never disagree about one.
//
// A near-vertical triangle projects onto XY to a sliver, and treating that
// sliver as a line segment is exact to within the triangle's own lean -- a
// centimetre for a facade, and always on the conservative side because the
// segment is the widest chord of the projection. That reduction is what makes
// the swept test affordable: the caller gets back a handful of 2D segments and
// resolves a circle against them, instead of doing solid-body work per
// triangle per substep.
//
// Walks the physics grid like the ground probe, over the cells the gather
// radius reaches, and only the sections whose box the car is inside are
// asked at all. A gather runs once per frame, not once per substep.
int model3d_walls_near(const Model3D* m, float x, float y, float radius,
	float zLo, float zHi, Model3DWall* out, int count, int max)
{
	if (count < 0)
		count = 0;
	if (!m || !m->verts || !m->indices || !out || count >= max)
		return count;

	const u8* vp = (const u8*)m->verts;
	const u16* idx = m->indices;
	PhysWalk pw;
	physWalkBegin(&pw, m, x - radius, x + radius, y - radius, y + radius);
	// Which of a full buffer's entries is the farthest, and therefore the one
	// a closer candidate replaces. Recomputed only when it is the one that
	// got replaced, which is the common case and keeps this off the hot path.
	int worst = -1;

	for (int t; (t = physWalkNext(&pw)) >= 0; ) {
		const int i = t * 3;
		const float* a = (const float*)(vp + (u32)idx[i]     * VERTEX_SIZE);
		const float* b = (const float*)(vp + (u32)idx[i + 1] * VERTEX_SIZE);
		const float* c = (const float*)(vp + (u32)idx[i + 2] * VERTEX_SIZE);

		// Cheap XY reject first, the bounds widened by the query radius.
		float txLo = a[0] < b[0] ? a[0] : b[0]; if (c[0] < txLo) txLo = c[0];
		if (x < txLo - radius) continue;
		float hi = a[0] > b[0] ? a[0] : b[0]; if (c[0] > hi) hi = c[0];
		if (x > hi + radius) continue;
		float lo = a[1] < b[1] ? a[1] : b[1]; if (c[1] < lo) lo = c[1];
		if (y < lo - radius) continue;
		hi = a[1] > b[1] ? a[1] : b[1]; if (c[1] > hi) hi = c[1];
		if (y > hi + radius) continue;
		if (!physWalkOwns(&pw, txLo, lo))
			continue;

		// Then the height band. A kerb face is a wall by slope and would stop
		// the car dead if it were allowed to; what excludes it is that it ends
		// below the body band the caller asks about, so the ground probe gets
		// to climb it instead.
		float zlo = a[2] < b[2] ? a[2] : b[2]; if (c[2] < zlo) zlo = c[2];
		if (zlo > zHi) continue;
		float zhi = a[2] > b[2] ? a[2] : b[2]; if (c[2] > zhi) zhi = c[2];
		if (zhi < zLo) continue;

		// Steepness, by the same arithmetic the ground probe uses: |det| is
		// twice the XY-projected area and the face normal's z scales with it,
		// so comparing squares keeps this to multiplies.
		float e1x = b[0] - a[0], e1y = b[1] - a[1];
		float e2x = c[0] - a[0], e2y = c[1] - a[1];
		float det = e1x * e2y - e1y * e2x;
		float nx = e1y * (c[2] - a[2]) - (b[2] - a[2]) * e2y;
		float ny = (b[2] - a[2]) * e2x - e1x * (c[2] - a[2]);
		float lenSq = nx * nx + ny * ny + det * det;
		if (lenSq <= 0.0f)
			continue;
		if (det * det >= WALK_NZ * WALK_NZ * lenSq)
			continue;   // drivable, so the ground probe's business, not this

		// Widest of the three XY edges is the sliver's chord.
		float dx = b[0] - a[0], dy = b[1] - a[1];
		float bestSq = dx * dx + dy * dy;
		float sx0 = a[0], sy0 = a[1], sx1 = b[0], sy1 = b[1];
		dx = c[0] - b[0]; dy = c[1] - b[1];
		float d2 = dx * dx + dy * dy;
		if (d2 > bestSq) { bestSq = d2; sx0 = b[0]; sy0 = b[1]; sx1 = c[0]; sy1 = c[1]; }
		dx = a[0] - c[0]; dy = a[1] - c[1];
		d2 = dx * dx + dy * dy;
		if (d2 > bestSq) { bestSq = d2; sx0 = c[0]; sy0 = c[1]; sx1 = a[0]; sy1 = a[1]; }
		if (bestSq <= 1e-8f)
			continue;   // a triangle standing on end: no XY extent to hit

		// Exact distance from the query point to that segment, so a triangle
		// caught by the box test but round the corner is not carried.
		float ex = sx1 - sx0, ey = sy1 - sy0;
		float t = ((x - sx0) * ex + (y - sy0) * ey) / bestSq;
		if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
		float px = x - (sx0 + t * ex), py = y - (sy0 + t * ey);
		float dist2 = px * px + py * py;
		if (dist2 > radius * radius)
			continue;

		Model3DWall* wseg;
		if (count < max) {
			wseg = &out[count++];
			worst = -1;
		} else {
			if (worst < 0) {
				worst = 0;
				for (int k = 1; k < max; k++)
					if (out[k].dist2 > out[worst].dist2)
						worst = k;
			}
			if (dist2 >= out[worst].dist2)
				continue;
			wseg = &out[worst];
			worst = -1;
		}
		wseg->x0 = sx0; wseg->y0 = sy0;
		wseg->x1 = sx1; wseg->y1 = sy1;
		wseg->zmin = zlo; wseg->zmax = zhi;
		wseg->dist2 = dist2;
		wseg->z0min = wseg->z1min = zlo;
		wseg->z0max = wseg->z1max = zhi;
	}

	return count;
}
