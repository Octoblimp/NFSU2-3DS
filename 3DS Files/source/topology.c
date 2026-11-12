// The retail collision world. See topology.h for why, and
// tools/topo_pack.py for the format this reads and where every field of it
// came from.
#include "topology.h"
#include "lz4dec.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TOPO_FILE     "WORLD.NFST"
#define TOPO_MAGIC    0x5453464e   // 'NFST' little-endian
#define TOPO_VERSION  3            // 3 added the trees, 2 the groups; 1 and 2 still load
#define TOPO_HDR      0x40
#define TOPO_POLY     32
#define TOPO_GROUP    32           // one of retail's topology-group records
#define TOPO_TREE     32
#define TOPO_OUTLINE  32

// The `kind` byte tools/topo_pack.py precomputes from retail's flags.
#define TK_QUAD          0x01
#define TK_WALL          0x02
#define TK_GROUND        0x04
#define TK_UNBOUNDED     0x08
#define TK_STACKED       0x10
#define TK_NO_EARLY_OUT  0x20

// And retail's own flags byte (record +3), which the per-section search reads
// as retail does, because where it stops depends on it.
#define RF_STACKED       0x01
#define RF_NO_ELEVATION  0x0a       // wall, or not road: NewGetElevation's stop
#define RF_NO_EARLY_OUT  0x40

// The constructor's substitute z for an unbounded wall (Speed.elf 0x80335efc).
#define UNBOUNDED_Z   2048.0f

// NewGetElevation stops searching once a candidate lands within this of the
// ceiling (0x8033612c). Everything below it is still considered; this only
// decides when to stop looking for something better.
#define STACK_WINDOW  4.0f

// TopologyCoordinate's numbers (TASKS.md #238).
#define ELEV_RADIUS      9.0f       // NewGetElevation's SetSectionPosition, 0x8033611c
#define INTEREST_RADIUS  0.0f       // SetInterestBBox's, 0x8033607c
#define NO_BEST_Z        -99999.0f  // NewGetElevation's starting best, 0x80336148
#define TREE_CELL        32.0f      // 0x80336140, and its inverse 0x80336130
#define TREE_CELL_INV    0.03125f
#define TREE_SUB_INV     0.125f     // the 8 m sub-cell, 0x80336144
#define NEAR_OUTLINE     0.1f       // FindBoundary's "close enough", 0x8033896c
#define FAR_OUTLINE      9999999.0f // FindClosestBoundary's start, 0x80338964
#define CLEAR_DIST       -999.0f    // VisibleSectionCoordinate::Clear, 0x80339034
#define CACHE_MAX        4          // the coordinate's recent polygons, +0x2c0

#define Q_XY   0.125f          // the s16s are eighths of a metre
#define Q_Z    0.00390625f     // and 256ths, relative to zbase

typedef struct {
	s16 x[4];
	s16 y[4];
	s16 z[4];
	s16 zbase;
	u8  kind;
	s8  terrain;
	u8  group;      // topology group; 0 is "always there", see topo_set_groups()
	u8  bbsel;      // four 2-bit vertex selectors for the XY bounding box
	u8  rflags;     // retail's own flags byte, kept verbatim
	u8  pad;
} TopoPoly;

// One of retail's TopologyTrees, as tools/topo_pack.py wrote it: the grid and
// where its cells, entries and polygons start in the shared arrays.
typedef struct {
	s16   number;       // the tree's own section: drivable section + lod
	u16   pad;
	u16   w, h;         // TopologyTree +0x2c, +0x2e
	float ox, oy;       // +0x30, +0x34
	u32   polyBase, polyCount;
	u32   firstCell, firstEntry;
} TopoTree;

// A drivable VisibleSectionBoundary.
typedef struct {
	s16   number;
	u16   numPoints;
	u32   firstPoint;
	float bbmin[2], bbmax[2];
	u32   pad[2];
} TopoOutline;

_Static_assert(sizeof(TopoPoly) == TOPO_POLY, "TopoPoly is the file's record");
_Static_assert(sizeof(TopoTree) == TOPO_TREE, "TopoTree is the file's record");
_Static_assert(sizeof(TopoOutline) == TOPO_OUTLINE, "TopoOutline is the file's record");

// TopologyCoordinate, the parts of it the queries use. The first four are its
// VisibleSectionCoordinate (+0xc): where it last looked, the outline that was
// in, and how far it can move before it has to look again.
typedef struct {
	float at[2];
	int   outline;        // into Topology.outlines, -1 none
	float dist;
	int   tree;           // into Topology.trees, -1 not connected
	u32   cache[CACHE_MAX];
	int   cacheN;
	bool  logSwitches;
	int   logged;         // switches logged since the last clear
} TopoCoord;

// Enough for a long drive; a car running along an outline's edge could
// otherwise fill the log with the same two sections.
#define SWITCH_LOG_MAX  200

struct Topology {
	u8*   blob;
	size_t bytes;

	const TopoPoly* polys;
	const u32*      cells;     // two u32 per cell: first entry, count
	const u32*      entries;   // polygon indices
	u32 polyCount, cellCount, entryCount;

	float ox, oy, cellSize, invCell;
	int   gw, gh;
	float bbmin[3], bbmax[3];

	// A polygon's bbox usually straddles a cell edge, so a query covering
	// several cells sees it several times -- and a wall gathered twice is
	// pushed against twice. One byte per polygon holding the generation that
	// last saw it costs 150 KB and makes the dedup a compare and a store.
	u8* stamp;
	u8  gen;

	// Retail's topology-group table, TopologyManager+0x82c, as topo_pack.py
	// copied it: per record a u32 bStringHash of the name at +0, the name
	// itself at +9. groupOn[] is the record's byte +8, the one thing the
	// queries read; a polygon index is a u8, so 256 covers every one.
	u8* groups;
	u32 groupCount;
	u8  groupOn[256];

	// Version 3: retail's per-section trees and the drivable outlines that
	// choose between them (#238). treeCount is 0 for an older file, and then
	// every query goes to the merged grid above.
	TopoTree*    trees;
	u8*          treeBlob;     // the cells, then the entries
	const u16*   tcells;       // {first, count}, first relative to the tree
	const u16*   tentries;     // {polygon, sub-cell mask}, relative to the tree
	u32          treeCount, tcellCount, tentryCount;
	int          lod;          // ScenerySectionLODOffset
	TopoOutline* outlines;
	float*       outlinePts;
	u32          outlineCount, outlinePtCount;
	// VisibleSectionManager's drivable list (+0), in its order: FindBoundary
	// moves the outline it finds to the front, so the next search starts with
	// the one the car is most likely still in. Shared by every coordinate,
	// as retail's one manager is.
	u16*         mru;

	TopoCoord coords[TOPO_COORD_COUNT];
};

static void (*s_log)(const char*) = NULL;

void topo_set_log(void (*sink)(const char* msg)) { s_log = sink; }

static void tlog(const char* fmt, ...)
{
	if (!s_log)
		return;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	s_log(buf);
}

static u32 rd32(const u8* p, int off)
{
	return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) |
	       ((u32)p[off + 3] << 24);
}

static float rdf32(const u8* p, int off)
{
	union { u32 u; float f; } c;
	c.u = rd32(p, off);
	return c.f;
}

// "O28" for 1528: letter * 100 + number, A being 1.
static void sectionName(int number, char out[8])
{
	if (number <= 0)
		snprintf(out, 8, "none");
	else
		snprintf(out, 8, "%c%02d", 'A' + number / 100 - 1, number % 100);
}

// --------------------------------------------------------------- dequantising

static inline float pxOf(const TopoPoly* p, int k) { return (float)p->x[k] * Q_XY; }
static inline float pyOf(const TopoPoly* p, int k) { return (float)p->y[k] * Q_XY; }

static inline float pzOf(const TopoPoly* p, int k)
{
	// An unbounded wall's four stored z values are not its geometry: the
	// constructor replaces them outright, which is what makes it a barrier
	// rather than a quad hanging in space.
	if (p->kind & TK_UNBOUNDED)
		return (k == 0 || k == 3) ? -UNBOUNDED_Z : UNBOUNDED_Z;
	return (float)p->zbase + (float)p->z[k] * Q_Z;
}

static inline int pnOf(const TopoPoly* p) { return (p->kind & TK_QUAD) ? 4 : 3; }

static inline void polyBox(const TopoPoly* p, float* x0, float* y0,
	float* x1, float* y1)
{
	const u8 b = p->bbsel;
	*x0 = (float)p->x[(b >> 0) & 3] * Q_XY;
	*y0 = (float)p->y[(b >> 2) & 3] * Q_XY;
	*x1 = (float)p->x[(b >> 4) & 3] * Q_XY;
	*y1 = (float)p->y[(b >> 6) & 3] * Q_XY;
}

// TrackPolygon::GetAmountOutside (0x801f0494): the largest signed edge
// distance, so <= 0 is inside and anything positive is how far outside the
// worst edge puts it. Retail stops at the first positive edge and starts the
// running maximum at -1, both reproduced here because callers compare the
// result against zero and nothing else.
static float amountOutside(const TopoPoly* p, float px, float py)
{
	const int n = pnOf(p);
	float worst = -1.0f;
	for (int k = 0; k < n; k++) {
		const int j = (k + 1 == n) ? 0 : k + 1;
		const float x0 = pxOf(p, k), y0 = pyOf(p, k);
		const float d = (pyOf(p, j) - y0) * (px - x0) -
		                (pxOf(p, j) - x0) * (py - y0);
		if (d > worst) {
			worst = d;
			if (worst > 0.0f)
				break;
		}
	}
	return worst;
}

// The plane through the first three vertices, unnormalised. The constructor
// stores a normalised one at +0x64 and its d at +0x70; the port keeps the
// quantised vertices instead and rebuilds it, which costs six multiplies on
// the handful of polygons that get this far.
static void polyPlane(const TopoPoly* p, float out[6])
{
	const float x0 = pxOf(p, 0), y0 = pyOf(p, 0), z0 = pzOf(p, 0);
	const float ax = pxOf(p, 1) - x0, ay = pyOf(p, 1) - y0, az = pzOf(p, 1) - z0;
	const float bx = pxOf(p, 2) - x0, by = pyOf(p, 2) - y0, bz = pzOf(p, 2) - z0;
	out[0] = ay * bz - az * by;
	out[1] = az * bx - ax * bz;
	out[2] = ax * by - ay * bx;
	out[3] = x0;
	out[4] = y0;
	out[5] = z0;
}

// The plane's height over (px, py); false when the polygon is edge-on and
// there is no height to read off it.
static bool planeZ(const TopoPoly* p, float px, float py, float* outZ)
{
	float pl[6];
	polyPlane(p, pl);
	if (pl[2] > -1.0e-6f && pl[2] < 1.0e-6f)
		return false;
	*outZ = pl[5] - (pl[0] * (px - pl[3]) + pl[1] * (py - pl[4])) / pl[2];
	return true;
}

// ---------------------------------------------------------------- open/close

static void coordClear(TopoCoord* c)
{
	// VisibleSectionCoordinate::Clear (0x803129a8): at the origin, in no
	// outline, and a distance that makes the next query look.
	c->at[0] = c->at[1] = 0.0f;
	c->outline = -1;
	c->dist = CLEAR_DIST;
	c->tree = -1;
	c->cacheN = 0;
	c->logged = 0;
}

static void freeTrees(Topology* t)
{
	if (t->treeBlob)
		linearFree(t->treeBlob);
	free(t->trees);
	free(t->outlines);
	free(t->outlinePts);
	free(t->mru);
	t->treeBlob = NULL;
	t->trees = NULL;
	t->outlines = NULL;
	t->outlinePts = NULL;
	t->mru = NULL;
	t->tcells = t->tentries = NULL;
	t->treeCount = t->outlineCount = 0;
	t->lod = 0;
}

// #270: WORLD.NFST is read front to back, and comes either whole or as LZ4
// blocks (tools/topo_pack.py): 'NFTL', version 1, the raw size, the raw block
// size, a u32 stored size per block (equal to the block's raw length: kept as
// is), then the blocks. LZ4-HC has the file at about 64%, 2.5 MB less to
// read off the card at every world entry, for ~30 ms of decoding.
#define TOPO_LZ4_MAGIC 0x4c54464e   // 'NFTL' little-endian
#define TOPO_LZ4_HDR   16

typedef struct {
	FILE* f;
	bool lz4;
	u32* sizes;
	u32 nBlocks, next, blockRaw, rawLeft;
	u8* comp;
	u8* raw;
	u32 have, pos;
	u32 rawSize, compSize;
} TopoIn;

static void tinClose(TopoIn* in)
{
	if (in->f)
		fclose(in->f);
	free(in->sizes);
	free(in->comp);
	free(in->raw);
	memset(in, 0, sizeof(*in));
}

// `f` (just opened) as either form. False on a damaged LZ4 header.
static bool tinAttach(TopoIn* in, FILE* f)
{
	memset(in, 0, sizeof(*in));
	in->f = f;
	u8 h[TOPO_LZ4_HDR];
	if (fread(h, 1, sizeof(h), f) != sizeof(h) || rd32(h, 0) != TOPO_LZ4_MAGIC) {
		fseek(f, 0, SEEK_SET);
		return true;
	}
	in->lz4 = true;
	in->rawSize = in->rawLeft = rd32(h, 8);
	in->blockRaw = rd32(h, 12);
	if (rd32(h, 4) != 1 || !in->rawSize || !in->blockRaw || in->blockRaw > (4u << 20))
		return false;
	in->nBlocks = (in->rawSize + in->blockRaw - 1) / in->blockRaw;
	in->sizes = (u32*)malloc((size_t)in->nBlocks * 4);
	if (!in->sizes || fread(in->sizes, 4, in->nBlocks, f) != in->nBlocks)
		return false;
	u32 most = 0;
	for (u32 i = 0; i < in->nBlocks; i++) {
		if (in->sizes[i] > most)
			most = in->sizes[i];
		in->compSize += in->sizes[i];
	}
	if (most > in->blockRaw + in->blockRaw / 255 + 16)
		return false;
	in->comp = (u8*)malloc(most ? most : 1);
	in->raw = (u8*)malloc(in->blockRaw);
	return in->comp && in->raw;
}

static bool tinBlock(TopoIn* in)
{
	if (in->next >= in->nBlocks || !in->rawLeft)
		return false;
	const u32 n = in->rawLeft < in->blockRaw ? in->rawLeft : in->blockRaw;
	const u32 c = in->sizes[in->next++];
	if (c == n) {
		if (fread(in->raw, 1, n, in->f) != n)
			return false;
	} else if (fread(in->comp, 1, c, in->f) != c || !lz4_decode(in->comp, c, in->raw, n)) {
		return false;
	}
	in->rawLeft -= n;
	in->have = n;
	in->pos = 0;
	return true;
}

// Exactly `n` bytes, or false.
static bool tinRead(TopoIn* in, void* dst, size_t n)
{
	if (!in->lz4)
		return fread(dst, 1, n, in->f) == n;
	u8* d = (u8*)dst;
	while (n) {
		if (in->pos == in->have && !tinBlock(in))
			return false;
		size_t k = in->have - in->pos;
		if (k > n)
			k = n;
		memcpy(d, in->raw + in->pos, k);
		in->pos += (u32)k;
		d += k;
		n -= k;
	}
	return true;
}

// Version 3's trees and outlines, which follow the group table. False when
// they are missing or do not hold together; the caller then keeps the merged
// grid, which is what an older file gets anyway.
static bool loadTrees(Topology* t, TopoIn* f)
{
	u8 h[16];
	if (!tinRead(f, h, sizeof(h)))
		return false;
	const u32 treeCount = rd32(h, 0), cellCount = rd32(h, 8), entryCount = rd32(h, 12);
	if (!treeCount || treeCount > 1024 || cellCount > (1u << 22) ||
	    entryCount > (1u << 24))
		return false;
	t->lod = (int)rd32(h, 4);

	const size_t cellBytes = (size_t)cellCount * 4, entryBytes = (size_t)entryCount * 4;
	t->trees    = (TopoTree*)malloc((size_t)treeCount * TOPO_TREE);
	t->treeBlob = (u8*)linearAlloc(cellBytes + entryBytes + 4);
	if (!t->trees || !t->treeBlob ||
	    !tinRead(f, t->trees, (size_t)(TOPO_TREE) * (treeCount)) ||
	    !tinRead(f, t->treeBlob, (size_t)(1) * (cellBytes + entryBytes)))
		return false;
	t->tcells      = (const u16*)t->treeBlob;
	t->tentries    = (const u16*)(t->treeBlob + cellBytes);
	t->treeCount   = treeCount;
	t->tcellCount  = cellCount;
	t->tentryCount = entryCount;

	if (!tinRead(f, h, sizeof(h)))
		return false;
	const u32 outlineCount = rd32(h, 0), ptCount = rd32(h, 4);
	if (!outlineCount || outlineCount > 4096 || ptCount > (1u << 20))
		return false;
	t->outlines   = (TopoOutline*)malloc((size_t)outlineCount * TOPO_OUTLINE);
	t->outlinePts = (float*)malloc((size_t)ptCount * 8 + 8);
	t->mru        = (u16*)malloc((size_t)outlineCount * sizeof(u16));
	if (!t->outlines || !t->outlinePts || !t->mru ||
	    !tinRead(f, t->outlines, (size_t)(TOPO_OUTLINE) * (outlineCount)) ||
	    !tinRead(f, t->outlinePts, (size_t)(8) * (ptCount)))
		return false;
	t->outlineCount   = outlineCount;
	t->outlinePtCount = ptCount;

	// Everything a query will follow has to land inside its own tree, so a
	// damaged file is refused here rather than read out of bounds later.
	for (u32 i = 0; i < treeCount; i++) {
		const TopoTree* tr = &t->trees[i];
		const u32 entryEnd = (i + 1 < treeCount) ? t->trees[i + 1].firstEntry : entryCount;
		if (!tr->w || !tr->h || tr->polyBase + tr->polyCount > t->polyCount ||
		    tr->firstCell + (u32)tr->w * tr->h > cellCount ||
		    tr->firstEntry > entryEnd || entryEnd > entryCount)
			return false;
		const u32 entries = entryEnd - tr->firstEntry;
		for (u32 c = 0; c < (u32)tr->w * tr->h; c++) {
			const u16* cell = t->tcells + (tr->firstCell + c) * 2;
			if ((u32)cell[0] + cell[1] > entries)
				return false;
		}
		for (u32 e = 0; e < entries; e++)
			if (t->tentries[(tr->firstEntry + e) * 2] >= tr->polyCount)
				return false;
	}
	for (u32 i = 0; i < outlineCount; i++) {
		const TopoOutline* o = &t->outlines[i];
		if (!o->numPoints || o->firstPoint + o->numPoints > ptCount)
			return false;
		// VisibleSectionManager::Loader adds them to the tail in file order.
		t->mru[i] = (u16)i;
	}
	t->bytes += sizeof(TopoTree) * treeCount + cellBytes + entryBytes +
	            sizeof(TopoOutline) * outlineCount + (size_t)ptCount * 8 +
	            outlineCount * sizeof(u16);
	return true;
}

Topology* topo_open(const char* dirPath)
{
	char path[400];
	snprintf(path, sizeof(path), "%s/%s", dirPath, TOPO_FILE);
	FILE* f = fopen(path, "rb");
	if (!f) {
		// Not an error: a pack made before #236 has no WORLD.NFST, and the
		// drawn-geometry scans still answer. Said out loud all the same,
		// because a card that is missing this file is a card that still has
		// the phantom walls, and nothing on screen would say so.
		tlog("topology: %s missing -- colliding against the drawn geometry "
		     "(run tools/topo_pack.py)", TOPO_FILE);
		return NULL;
	}

	TopoIn tin;
	if (!tinAttach(&tin, f)) {
		tinClose(&tin);
		tlog("topology: %s's LZ4 header is damaged -- drawn geometry used instead",
		     TOPO_FILE);
		return NULL;
	}
	if (tin.lz4)
		tlog("topology: %s is LZ4, %u KB on the card for %u KB", TOPO_FILE,
		     (unsigned)(tin.compSize >> 10), (unsigned)(tin.rawSize >> 10));

	u8 hdr[TOPO_HDR];
	if (!tinRead(&tin, hdr, TOPO_HDR) ||
	    rd32(hdr, 0) != TOPO_MAGIC || rd32(hdr, 4) < 1 ||
	    rd32(hdr, 4) > TOPO_VERSION) {
		tinClose(&tin);
		tlog("topology: %s present but not a v%d NFST -- drawn geometry used "
		     "instead (run tools/topo_pack.py)", TOPO_FILE, TOPO_VERSION);
		return NULL;
	}
	const u32 version = rd32(hdr, 4);

	const u32 polyCount  = rd32(hdr, 0x08);
	const u32 cellCount  = rd32(hdr, 0x0c);
	const u32 entryCount = rd32(hdr, 0x10);
	const int gw = (int)rd32(hdr, 0x20);
	const int gh = (int)rd32(hdr, 0x24);
	if (!polyCount || !cellCount || gw <= 0 || gh <= 0 ||
	    (u32)(gw * gh) != cellCount) {
		tinClose(&tin);
		tlog("topology: %s header is inconsistent -- ignored", TOPO_FILE);
		return NULL;
	}

	const size_t polyBytes  = (size_t)polyCount * TOPO_POLY;
	const size_t cellBytes  = (size_t)cellCount * 8;
	const size_t entryBytes = (size_t)entryCount * 4;
	const size_t want = polyBytes + cellBytes + entryBytes;

	// The linear heap, not malloc: this is five and a half megabytes and the
	// malloc heap is 28, while linear has a hundred spare after the world's
	// own budget. Nothing here is touched by the GPU; linearAlloc is simply
	// where the room is.
	u8* blob = (u8*)linearAlloc(want);
	if (!blob) {
		tinClose(&tin);
		tlog("topology: no room for %u KB -- drawn geometry used instead",
		     (unsigned)(want >> 10));
		return NULL;
	}
	if (!tinRead(&tin, blob, want)) {
		linearFree(blob);
		tinClose(&tin);
		tlog("topology: %s is short -- ignored", TOPO_FILE);
		return NULL;
	}

	// Version 2 follows the entries with the group table: a u32 count, 12
	// bytes of padding, then the records. A table that is missing or does
	// not read back whole leaves groupCount 0, handled below.
	u8* groups = NULL;
	u32 groupCount = 0;
	u8 ghdr[16];
	if (version >= 2 && tinRead(&tin, ghdr, sizeof(ghdr))) {
		const u32 n = rd32(ghdr, 0);
		if (n > 0 && n <= 256 && (groups = (u8*)malloc((size_t)n * TOPO_GROUP))) {
			if (tinRead(&tin, groups, (size_t)TOPO_GROUP * n)) {
				groupCount = n;
			} else {
				free(groups);
				groups = NULL;
			}
		}
	}

	Topology* t = (Topology*)calloc(1, sizeof(Topology));
	if (!t) {
		tinClose(&tin);
		free(groups);
		linearFree(blob);
		return NULL;
	}
	t->stamp = (u8*)calloc(polyCount, 1);
	if (!t->stamp) {
		tinClose(&tin);
		free(groups);
		linearFree(blob);
		free(t);
		return NULL;
	}
	t->groups     = groups;
	t->groupCount = groupCount;
	if (groupCount) {
		// TopologyManager::Init (Speed.elf 0x801f33fc): record 0 on, every
		// other one off, until topo_set_groups() says which race is on.
		t->groupOn[0] = 1;
	} else {
		// A version 1 pack has no table, so there is no telling one race's
		// barriers from another's and they all stay solid -- the phantom
		// walls of TASKS.md #232. Said out loud for the same reason as a
		// missing file.
		memset(t->groupOn, 1, sizeof(t->groupOn));
		tlog("topology: %s has no group table -- every race's barriers are "
		     "solid (re-run tools/topo_pack.py)", TOPO_FILE);
	}

	t->blob       = blob;
	t->bytes      = want + polyCount;
	t->polys      = (const TopoPoly*)blob;
	t->cells      = (const u32*)(blob + polyBytes);
	t->entries    = (const u32*)(blob + polyBytes + cellBytes);
	t->polyCount  = polyCount;
	t->cellCount  = cellCount;
	t->entryCount = entryCount;
	t->ox         = rdf32(hdr, 0x14);
	t->oy         = rdf32(hdr, 0x18);
	t->cellSize   = rdf32(hdr, 0x1c);
	t->invCell    = (t->cellSize > 0.0f) ? 1.0f / t->cellSize : 0.03125f;
	t->gw         = gw;
	t->gh         = gh;
	for (int i = 0; i < 3; i++) {
		t->bbmin[i] = rdf32(hdr, 0x28 + i * 4);
		t->bbmax[i] = rdf32(hdr, 0x34 + i * 4);
	}
	t->gen = 0;

	if (version >= 3 && groupCount) {
		if (!loadTrees(t, &tin)) {
			freeTrees(t);
			tlog("topology: %s's section trees did not load -- one merged "
			     "grid instead, and stacked roads can lift the car", TOPO_FILE);
		}
	} else {
		// The grid every version before 3 had: one for the whole world. On
		// a road with another above it, a section's tree can say "this is
		// the only surface" about the upper one while another section's
		// says both are stacked, and the merged grid cannot keep them apart
		// (#238). Said out loud, like the rest.
		tlog("topology: %s is v%u, without section trees -- stacked roads can "
		     "lift the car (re-run tools/topo_pack.py)", TOPO_FILE, (unsigned)version);
	}
	tinClose(&tin);

	for (int i = 0; i < TOPO_COORD_COUNT; i++)
		coordClear(&t->coords[i]);
	t->coords[TOPO_COORD_CAR].logSwitches = true;

	u32 walls = 0, road = 0;
	for (u32 i = 0; i < polyCount; i++) {
		if (t->polys[i].kind & TK_WALL)
			walls++;
		else if (t->polys[i].kind & TK_GROUND)
			road++;
	}
	tlog("topology: %u polys (%u solid, %u road), %dx%d@%.0fm %u refs, "
	     "%u groups, %u trees over %u drivable outlines (lod %d), %u KB",
	     polyCount, walls, road, gw, gh, t->cellSize, entryCount,
	     (unsigned)groupCount, (unsigned)t->treeCount,
	     (unsigned)t->outlineCount, t->lod, (unsigned)(t->bytes >> 10));
	return t;
}

void topo_close(Topology* t)
{
	if (!t)
		return;
	if (t->blob)
		linearFree(t->blob);
	freeTrees(t);
	free(t->groups);
	free(t->stamp);
	free(t);
}

// ------------------------------------------------------------------ groups

// bStringHash (Speed.elf 0x8005dbe0), the hash the table is keyed by.
static u32 bStringHash(const char* s)
{
	u32 h = 0xFFFFFFFFu;
	while (*s)
		h = h * 33u + (u8)*s++;
	return h;
}

// The global EnableGroup(name) (0x80196bd8), the part of it that touches
// topology. It goes through VisibleSectionManager::GetGroupInfo (0x8020bec4)
// first, which matches the name's start against VisibleGroupInfoTable
// (0x8036fd84) without regard to case; only a group whose entry's second word
// is non-zero reaches the topology table. That table is three entries:
// "BARRIERS_" 1, "PLAYER_BARRIERS_" 1, "FREE_ROAM" 0 -- so the FREE_ROAM
// groups are scenery only. Then TopologyManager::GetGroupInfo (0x801f39e8)
// looks the hash up from record 1, never 0. True when a group was switched on.
static bool enableGroup(Topology* t, const char* name)
{
	if (strncasecmp(name, "BARRIERS_", 9) != 0 &&
	    strncasecmp(name, "PLAYER_BARRIERS_", 16) != 0)
		return false;
	const u32 h = bStringHash(name);
	for (u32 i = 1; i < t->groupCount; i++) {
		if (rd32(t->groups + i * TOPO_GROUP, 0) == h) {
			t->groupOn[i] = 1;
			return true;
		}
	}
	return false;
}

int topo_set_groups(Topology* t, int trackId, int careerStage)
{
	if (!t)
		return 0;

	// This is called where retail builds the race's cars, and with them
	// their TopologyCoordinates: free roam, and each race. A new coordinate
	// starts cleared, with nothing in its recent-polygon list -- which also
	// keeps a polygon of a group about to go off from being found there.
	for (int i = 0; i < TOPO_COORD_COUNT; i++)
		coordClear(&t->coords[i]);

	if (!t->groupCount)
		return 0;

	memset(t->groupOn, 0, sizeof(t->groupOn));
	t->groupOn[0] = 1;

	// TrackLoader::InitTopologyAndSceneryGroups (0x80196cb8) first sets the
	// groups named after the track-specific scenery sections, Z1..Z19. L4RA's
	// table has none of those names -- every record past 0 is a barrier set --
	// so that loop finds nothing here and is not repeated.
	//
	// Then the race's two barrier sets. The player's car asks for the same
	// two again through its own TopologyCoordinate (Car::SetupTopologyGroups,
	// 0x80221858), which switches them on around each of its queries.
	char name[32];
	int on = 0;
	snprintf(name, sizeof(name), "BARRIERS_%d", trackId);
	on += enableGroup(t, name);
	snprintf(name, sizeof(name), "PLAYER_BARRIERS_%d", trackId);
	on += enableGroup(t, name);
	// And outside a race, in career, the barriers for the career's stage.
	if (careerStage >= 0) {
		snprintf(name, sizeof(name), "BARRIERS_CAREER%d", careerStage);
		on += enableGroup(t, name);
	}

	char list[160];
	int len = 0;
	list[0] = '\0';
	for (u32 i = 1; i < t->groupCount && len < (int)sizeof(list) - 1; i++)
		if (t->groupOn[i])
			len += snprintf(list + len, sizeof(list) - len, " %.22s",
			                (const char*)(t->groups + i * TOPO_GROUP + 9));
	if (careerStage >= 0)
		tlog("topology: groups for track %d, career stage %d -- %d of %u on:%s",
		     trackId, careerStage, on, (unsigned)t->groupCount - 1,
		     on ? list : " none");
	else
		tlog("topology: groups for track %d -- %d of %u on:%s", trackId, on,
		     (unsigned)t->groupCount - 1, on ? list : " none");
	return on;
}

size_t topo_bytes(const Topology* t) { return t ? t->bytes : 0; }

// --------------------------------------------------------- section outlines
//
// Which section's tree a query reads (TASKS.md #238). Every query goes
// through a TopologyCoordinate, and the coordinate is connected to one tree
// at a time: the one belonging to the drivable section it is in.

static inline const float* olPts(const Topology* t, const TopoOutline* o)
{
	return t->outlinePts + (size_t)o->firstPoint * 2;
}

// VisibleSectionBoundary::IsPointInside (0x8020a6cc): the bbox, with no
// margin (0x80338914 is 0), then MyIsPointInPoly (0x8020a610), an even-odd
// crossing count with the edge's lower end in and its upper end out.
static bool olInside(const Topology* t, const TopoOutline* o, float x, float y)
{
	if (x < o->bbmin[0] || x > o->bbmax[0] || y < o->bbmin[1] || y > o->bbmax[1])
		return false;
	const float* v = olPts(t, o);
	const int n = o->numPoints;
	bool in = false;
	for (int i = 0, j = n - 1; i < n; j = i++) {
		const float xi = v[i * 2], yi = v[i * 2 + 1];
		const float xj = v[j * 2], yj = v[j * 2 + 1];
		if (((yi <= y && y < yj) || (yj <= y && y < yi)) &&
		    x < (xj - xi) * (y - yi) / (yj - yi) + xi)
			in = !in;
	}
	return in;
}

// The distance from a point to one outline edge, the way both of retail's
// boundary distances work it out: along the edge's direction and across it,
// then the nearer end or the line.
static float olEdgeDist(const float* a, const float* b, float px, float py)
{
	float ex = b[0] - a[0], ey = b[1] - a[1];
	const float wx = px - a[0], wy = py - a[1];
	const float len2 = ex * ex + ey * ey;
	if (len2 <= 5.0e-11f)
		return sqrtf(wx * wx + wy * wy);
	const float len = sqrtf(len2);
	ex /= len;
	ey /= len;
	const float along = ex * wx + ey * wy;
	const float across = ey * wx - ex * wy;
	if (along < 0.0f)
		return sqrtf(along * along + across * across);
	if (along > len) {
		const float d = along - len;
		return sqrtf(d * d + across * across);
	}
	return fabsf(across);
}

// GetDistanceInside (0x8020a9d0): 0 outside, otherwise the nearest edge,
// starting from the distance to the first point.
static float olDistanceInside(const Topology* t, const TopoOutline* o, float x, float y)
{
	if (!olInside(t, o, x, y))
		return 0.0f;
	const float* v = olPts(t, o);
	const int n = o->numPoints;
	const float dx = x - v[0], dy = y - v[1];
	float d = sqrtf(dx * dx + dy * dy);
	for (int i = 0; i < n; i++) {
		const int j = (i + 1 == n) ? 0 : i + 1;
		const float e = olEdgeDist(v + i * 2, v + j * 2, x, y);
		if (e < d)
			d = e;
	}
	return d;
}

// GetDistanceOutside (0x8020a730). `maxDist` is the best distance found so
// far, and an outline whose bbox, widened by it, does not hold the point
// answers exactly that -- the tie FindClosestBoundary then breaks by number.
static float olDistanceOutside(const Topology* t, const TopoOutline* o,
	float x, float y, float maxDist)
{
	if (x < o->bbmin[0] - maxDist || x > o->bbmax[0] + maxDist ||
	    y < o->bbmin[1] - maxDist || y > o->bbmax[1] + maxDist)
		return maxDist;
	if (olInside(t, o, x, y))
		return 0.0f;
	const float* v = olPts(t, o);
	const int n = o->numPoints;
	float d = maxDist;
	for (int i = 0; i < n; i++) {
		const int j = (i + 1 == n) ? 0 : i + 1;
		const float e = olEdgeDist(v + i * 2, v + j * 2, x, y);
		if (e < d)
			d = e;
	}
	return d;
}

static void mruToFront(Topology* t, u32 i)
{
	const u16 o = t->mru[i];
	memmove(t->mru + 1, t->mru, i * sizeof(u16));
	t->mru[0] = o;
}

// VisibleSectionManager::FindClosestBoundary (0x8020bc2c). An outline that
// holds the point is the answer at distance 0. Otherwise the nearest one --
// and on equal distance the lower section number, which with the widened
// bbox above can hand back an outline farther away than the one it replaces.
// That is retail's, and kept.
static int findClosestOutline(Topology* t, float x, float y, float* outDist)
{
	float best = FAR_OUTLINE;
	int bi = -1;
	for (u32 i = 0; i < t->outlineCount; i++) {
		const int o = t->mru[i];
		const TopoOutline* ol = &t->outlines[o];
		if (olInside(t, ol, x, y)) {
			mruToFront(t, i);
			*outDist = 0.0f;
			return o;
		}
		const float d = olDistanceOutside(t, ol, x, y, best);
		if (bi < 0 || d < best ||
		    (d == best && ol->number < t->outlines[bi].number)) {
			bi = o;
			best = d;
		}
	}
	*outDist = best;
	return bi;
}

// VisibleSectionManager::FindBoundary (0x8020bd1c): the outline holding the
// point, or the closest one if it is within 0.1 m, or none.
static int findOutline(Topology* t, float x, float y)
{
	for (u32 i = 0; i < t->outlineCount; i++) {
		const int o = t->mru[i];
		if (olInside(t, &t->outlines[o], x, y)) {
			mruToFront(t, i);
			return o;
		}
	}
	float d;
	const int o = findClosestOutline(t, x, y, &d);
	return (d < NEAR_OUTLINE) ? o : -1;
}

// VisibleSectionCoordinate::FullUpdate (0x8020acc0). Moves the coordinate
// once it has gone farther than its distance inside the outline plus
// `radius`; true when that changed the outline.
static bool fullUpdate(Topology* t, TopoCoord* c, float x, float y, float radius)
{
	const float dx = x - c->at[0], dy = y - c->at[1];
	const float d2 = dx * dx + dy * dy;
	const float moved = (d2 > 5.0e-11f) ? sqrtf(d2) : 0.0f;
	if (!(c->dist + radius < moved))
		return false;
	c->at[0] = x;
	c->at[1] = y;
	if (c->outline >= 0 && olInside(t, &t->outlines[c->outline], x, y)) {
		c->dist = olDistanceInside(t, &t->outlines[c->outline], x, y);
		return false;
	}
	c->outline = findOutline(t, x, y);
	if (c->outline < 0)
		findClosestOutline(t, x, y, &c->dist);   // how far until it is worth looking again
	else
		c->dist = olDistanceInside(t, &t->outlines[c->outline], x, y);
	return true;
}

// The tree for drivable section `section`, the one TopologyManager::GetIndex
// (0x801f3870) files it under: its own number less the LOD offset. -1 for
// section 0, whose slot is empty, and for a section with no tree.
static int treeFor(const Topology* t, int section)
{
	if (section <= 0)
		return -1;
	for (u32 i = 0; i < t->treeCount; i++)
		if (t->trees[i].number - t->lod == section)
			return (int)i;
	return -1;
}

// ConnectToTree (0x801f0e6c), which disconnects first: the recent-polygon
// list belongs to the old tree and goes with it.
static void connectTree(Topology* t, TopoCoord* c, int tree)
{
	c->tree = tree;
	c->cacheN = 0;
	if (!c->logSwitches || c->logged > SWITCH_LOG_MAX)
		return;
	if (++c->logged > SWITCH_LOG_MAX) {
		tlog("topology: car's further section changes not logged");
		return;
	}
	char name[8], treeName[8];
	if (tree >= 0) {
		sectionName(t->trees[tree].number - t->lod, name);
		sectionName(t->trees[tree].number, treeName);
		tlog("topology: car on %s's tree (%s) at %.0f,%.0f", name, treeName,
		     c->at[0], c->at[1]);
	} else {
		// Retail has no ground here at all. The port falls back to the
		// merged grid until the car is back inside a section, rather than
		// hold it in the air with nothing to stop it.
		tlog("topology: car outside every drivable section at %.0f,%.0f -- "
		     "merged grid until it is back", c->at[0], c->at[1]);
	}
}

// TopologyCoordinate::SetSectionPosition (0x801f1bb0). A cheap Manhattan
// test first, then FullUpdate, then a new tree if the section changed.
static void setSectionPosition(Topology* t, TopoCoord* c, float x, float y, float radius)
{
	if (!(c->dist + radius < fabsf(x - c->at[0]) + fabsf(y - c->at[1])))
		return;
	if (!fullUpdate(t, c, x, y, radius))
		return;
	const int section = (c->outline >= 0) ? t->outlines[c->outline].number : 0;
	if (c->tree < 0) {
		if (section == 0)
			return;
		const int tree = treeFor(t, section);
		if (tree >= 0)
			connectTree(t, c, tree);
	} else if (t->trees[c->tree].number - t->lod != section) {
		connectTree(t, c, treeFor(t, section));
	}
}

// The tree a query on `coord` at (x, y) reads, -1 for the merged grid.
static int coordTree(Topology* t, int coord, float x, float y, float radius)
{
	if (!t->treeCount || coord < 0 || coord >= TOPO_COORD_COUNT)
		return -1;
	TopoCoord* c = &t->coords[coord];
	setSectionPosition(t, c, x, y, radius);
	return c->tree;
}

int topo_coord_section(const Topology* t, int coord)
{
	if (!t || !t->treeCount || coord < 0 || coord >= TOPO_COORD_COUNT)
		return -1;
	const TopoCoord* c = &t->coords[coord];
	return (c->tree >= 0) ? t->trees[c->tree].number - t->lod : 0;
}

int topo_coord_tree_section(const Topology* t, int coord)
{
	if (!t || !t->treeCount || coord < 0 || coord >= TOPO_COORD_COUNT)
		return -1;
	const TopoCoord* c = &t->coords[coord];
	return (c->tree >= 0) ? t->trees[c->tree].number : 0;
}

// ------------------------------------------------------------------ the grid

static inline int cellOf(const Topology* t, float x, float y)
{
	const int cx = (int)floorf((x - t->ox) * t->invCell);
	const int cy = (int)floorf((y - t->oy) * t->invCell);
	if (cx < 0 || cy < 0 || cx >= t->gw || cy >= t->gh)
		return -1;
	return cy * t->gw + cx;
}

// --------------------------------------------------------------- ground probe

// One point, one pass, on the merged grid: an older file, or a coordinate
// outside every drivable section. `ceiling` is the highest surface that may
// be taken; the caller runs this twice, the second time with no ceiling at
// all, exactly as NewGetElevation does.
static const TopoPoly* groundAt(const Topology* t, float px, float py,
	float ceiling, float* outZ)
{
	const int c = cellOf(t, px, py);
	if (c < 0)
		return NULL;
	const u32 first = t->cells[c * 2];
	const u32 count = t->cells[c * 2 + 1];

	const TopoPoly* best = NULL;
	float bestZ = -1.0e30f;

	for (u32 e = 0; e < count; e++) {
		const TopoPoly* p = &t->polys[t->entries[first + e]];

		// Retail's first test, before anything is computed: `flags & 0x0a`
		// takes out walls and the surfaces that exist for the solver but are
		// not road -- roof tops, verges behind fences, interiors.
		if (!(p->kind & TK_GROUND))
			continue;

		// Then the group: NewGetElevation skips a polygon whose group record
		// has byte +8 clear, so another race's barriers carry no road either.
		if (!t->groupOn[p->group])
			continue;

		float bx0, by0, bx1, by1;
		polyBox(p, &bx0, &by0, &bx1, &by1);
		if (px < bx0 || px > bx1 || py < by0 || py > by1)
			continue;

		const float out = amountOutside(p, px, py);
		if (out > 0.0f)
			continue;

		float z;
		if (!planeZ(p, px, py, &z))
			continue;              // edge-on: no z to read off it

		// A polygon that is not part of a stack and strictly contains the
		// point is the answer, whatever its height. This is the case almost
		// every query takes: on an ordinary street there is one surface, and
		// retail does not pay to prove it.
		if (!(p->kind & TK_STACKED) && out < 0.0f) {
			*outZ = z;
			return p;
		}

		if (z <= bestZ || z > ceiling)
			continue;
		best = p;
		bestZ = z;

		// Close enough under the car to stop looking -- unless the polygon
		// asks not to be the end of the search.
		if (!(p->kind & TK_NO_EARLY_OUT) && (ceiling - z) < STACK_WINDOW)
			break;
	}

	if (best)
		*outZ = bestZ;
	return best;
}

// The rest of NewGetElevation (0x801f1484), on the coordinate's own tree:
// its recent polygons, then the tree's cell, both passes.
static const TopoPoly* treeGroundAt(Topology* t, TopoCoord* c, float px,
	float py, float ceiling, float* outZ)
{
	// The last four polygons it stood on, most recent first. One that
	// strictly holds the point is taken if it is not part of a stack, or if
	// it is and sits within 4 m under the ceiling -- on a straight road that
	// is the whole query.
	for (int i = 0; i < c->cacheN; i++) {
		const TopoPoly* p = &t->polys[c->cache[i]];
		if (!(amountOutside(p, px, py) < 0.0f))
			continue;
		float z;
		if (!planeZ(p, px, py, &z))
			continue;
		if ((p->rflags & RF_STACKED) && !(z <= ceiling && ceiling - z < STACK_WINDOW))
			continue;
		const u32 idx = c->cache[i];
		for (int k = i; k > 0; k--)
			c->cache[k] = c->cache[k - 1];
		c->cache[0] = idx;
		*outZ = z;
		return p;
	}

	// The cell, and the 8 m sub-cell inside it as a bit: column + row * 4.
	// Retail truncates rather than floors, and works out the sub-cell from
	// the cell before clamping it to the grid; both kept. A point left of or
	// below its sub-cell gets no bit, as the hardware's shift gives none.
	const TopoTree* tr = &t->trees[c->tree];
	int cx = (int)((px - tr->ox) * TREE_CELL_INV);
	int cy = (int)((py - tr->oy) * TREE_CELL_INV);
	const int sx = (int)((px - ((float)cx * TREE_CELL + tr->ox)) * TREE_SUB_INV);
	const int sy = (int)((py - ((float)cy * TREE_CELL + tr->oy)) * TREE_SUB_INV);
	const u32 mask = (sx >= 0 && sx < 4 && sy >= 0 && sy < 4) ? (1u << sx) << (sy * 4) : 0;
	if (cx < 0) cx = 0;
	if (cx > tr->w - 1) cx = tr->w - 1;
	if (cy < 0) cy = 0;
	if (cy > tr->h - 1) cy = tr->h - 1;
	const u16* cell = t->tcells + (tr->firstCell + (u32)(cy * tr->w + cx)) * 2;
	const u16* ent = t->tentries + (tr->firstEntry + cell[0]) * 2;
	const u32 count = cell[1];

	const TopoPoly* hit = NULL;
	float hitZ = 0.0f;
	const TopoPoly* best = NULL;
	float bestZ = NO_BEST_Z;
	for (int pass = 0; pass < 2 && !hit; pass++) {
		for (u32 e = 0; e < count; e++) {
			if (!(ent[e * 2 + 1] & mask))
				continue;
			const TopoPoly* p = &t->polys[tr->polyBase + ent[e * 2]];

			// Every cell lists its road first, so the first wall or non-road
			// polygon is the end of the road. Retail stops there rather than
			// skips, and so does this.
			if (p->rflags & RF_NO_ELEVATION)
				break;

			float bx0, by0, bx1, by1;
			polyBox(p, &bx0, &by0, &bx1, &by1);
			if (px < bx0 || py < by0 || px > bx1 || py > by1)
				continue;
			if (!t->groupOn[p->group])
				continue;
			if (!(p->kind & TK_GROUND))
				continue;          // a record with no area, see topo_pack.py

			const float out = amountOutside(p, px, py);
			if (out > 0.0f)
				continue;
			float z;
			if (!planeZ(p, px, py, &z))
				continue;

			// Not part of a stack and strictly inside: taken on sight. In
			// this tree that is a promise there is nothing above or below it.
			if (!(p->rflags & RF_STACKED) && out < 0.0f) {
				hit = p;
				hitZ = z;
				break;
			}
			if (z > bestZ && z <= ceiling) {
				best = p;
				bestZ = z;
				if (!(p->rflags & RF_NO_EARLY_OUT) && ceiling - z < STACK_WINDOW)
					break;
			}
		}
		if (!hit && best) {
			hit = best;
			hitZ = bestZ;
		}
		// Nothing at or under the car: again with no ceiling (0x80336150),
		// so a car that has got underneath the road climbs back out.
		ceiling = 3.4028235e38f;
	}
	if (!hit)
		return NULL;

	// Into the recent list -- except a polygon that asked the search to keep
	// looking, which would not be a safe answer for the next query either.
	if (!(hit->rflags & RF_NO_EARLY_OUT)) {
		if (c->cacheN < CACHE_MAX)
			c->cacheN++;
		for (int k = c->cacheN - 1; k > 0; k--)
			c->cache[k] = c->cache[k - 1];
		c->cache[0] = (u32)(hit - t->polys);
	}
	*outZ = hitZ;
	return hit;
}

int topo_ground_probe(Topology* t, int coord, const float* xy, int n, float zMax,
	float* outZ, bool* outHit, float* outN, signed char* outTerrain)
{
	if (!t || n <= 0)
		return 0;

	int hits = 0;
	for (int i = 0; i < n; i++) {
		const float px = xy[i * 2], py = xy[i * 2 + 1];
		float z = 0.0f;
		const TopoPoly* p;
		const int tree = coordTree(t, coord, px, py, ELEV_RADIUS);
		if (tree >= 0) {
			p = treeGroundAt(t, &t->coords[coord], px, py, zMax, &z);
		} else {
			p = groundAt(t, px, py, zMax, &z);
			if (!p) {
				// Nothing at or below the car. Retail runs the whole search
				// again with the ceiling removed rather than reporting no
				// ground, so a car that has got underneath the road still
				// has something to stand on and climbs back out instead of
				// falling forever.
				p = groundAt(t, px, py, 3.4028235e38f, &z);
			}
		}
		if (!p)
			continue;

		// Accumulates like model3d_ground_z_multi(): a point that already has
		// a higher surface from another source keeps it.
		if (outHit[i] && z <= outZ[i])
			continue;

		outZ[i] = z;
		if (!outHit[i])
			hits++;
		outHit[i] = true;

		if (outN) {
			float pl[6];
			polyPlane(p, pl);
			float len = sqrtf(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
			if (len < 1.0e-9f) {
				outN[i * 3] = outN[i * 3 + 1] = 0.0f;
				outN[i * 3 + 2] = 1.0f;
			} else {
				// Point it up: the data's winding is not guaranteed to.
				if (pl[2] < 0.0f)
					len = -len;
				outN[i * 3]     = pl[0] / len;
				outN[i * 3 + 1] = pl[1] / len;
				outN[i * 3 + 2] = pl[2] / len;
			}
		}
		if (outTerrain)
			outTerrain[i] = p->terrain;
	}
	return hits;
}

// ----------------------------------------------------------------- the walls

// The widest XY separation between any two of the polygon's vertices. A wall
// is near-vertical by construction -- all 39,487 of L4RA's are within 5.7
// degrees of it -- so its vertices project onto a line, and that line between
// its two extremes is the segment the car has to be stopped by.
static void wallSegment(const TopoPoly* p, float* x0, float* y0,
	float* x1, float* y1)
{
	const int n = pnOf(p);
	float bestD = -1.0f;
	int bi = 0, bj = (n > 1) ? 1 : 0;
	for (int a = 0; a < n; a++) {
		for (int b = a + 1; b < n; b++) {
			const float dx = pxOf(p, b) - pxOf(p, a);
			const float dy = pyOf(p, b) - pyOf(p, a);
			const float d = dx * dx + dy * dy;
			if (d > bestD) {
				bestD = d;
				bi = a;
				bj = b;
			}
		}
	}
	*x0 = pxOf(p, bi);
	*y0 = pyOf(p, bi);
	*x1 = pxOf(p, bj);
	*y1 = pyOf(p, bj);
}

static void wallBand(const TopoPoly* p, float* lo, float* hi)
{
	if (p->kind & TK_UNBOUNDED) {
		*lo = -UNBOUNDED_Z;
		*hi = UNBOUNDED_Z;
		return;
	}
	const int n = pnOf(p);
	s16 mn = p->z[0], mx = p->z[0];
	for (int k = 1; k < n; k++) {
		if (p->z[k] < mn) mn = p->z[k];
		if (p->z[k] > mx) mx = p->z[k];
	}
	*lo = (float)p->zbase + (float)mn * Q_Z;
	*hi = (float)p->zbase + (float)mx * Q_Z;
}

static float segDist2(float x0, float y0, float x1, float y1, float px, float py)
{
	const float ex = x1 - x0, ey = y1 - y0;
	const float e2 = ex * ex + ey * ey;
	float u = 0.0f;
	if (e2 > 1.0e-8f) {
		u = ((px - x0) * ex + (py - y0) * ey) / e2;
		if (u < 0.0f) u = 0.0f;
		else if (u > 1.0f) u = 1.0f;
	}
	const float dx = px - (x0 + ex * u), dy = py - (y0 + ey * u);
	return dx * dx + dy * dy;
}

// The polygon's outline across fraction u of its wall segment (s0, along e,
// e2 = |e|^2): the lowest and highest z it reaches there. False when the
// outline does not reach u.
#define FACE_EDGE_M  0.05f   // an outline edge this short along the segment stands upright

static bool faceBandAt(const TopoPoly* p, float s0x, float s0y, float ex, float ey,
	float e2, float u, float* lo, float* hi)
{
	const int n = pnOf(p);
	const float eps = FACE_EDGE_M / sqrtf(e2);
	float pu[4], pz[4];
	for (int k = 0; k < n; k++) {
		pu[k] = ((pxOf(p, k) - s0x) * ex + (pyOf(p, k) - s0y) * ey) / e2;
		pz[k] = pzOf(p, k);
	}
	bool any = false;
	float mn = 0.0f, mx = 0.0f;
	for (int k = 0; k < n; k++) {
		const int j = (k + 1 == n) ? 0 : k + 1;
		float za, zb;
		if (fabsf(pu[j] - pu[k]) <= eps) {
			if (fabsf(u - pu[k]) > eps)
				continue;
			za = pz[k];
			zb = pz[j];
		} else {
			float f = (u - pu[k]) / (pu[j] - pu[k]);
			if (f < -eps || f > 1.0f + eps)
				continue;
			f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
			za = zb = pz[k] + (pz[j] - pz[k]) * f;
		}
		const float a = za < zb ? za : zb, b = za < zb ? zb : za;
		if (!any || a < mn) mn = a;
		if (!any || b > mx) mx = b;
		any = true;
	}
	if (any) {
		*lo = mn;
		*hi = mx;
	}
	return any;
}

// PORT (TASKS.md #280): the stretch of a wall within the query's reach, and
// the face's own band at each end of it. The whole quad's band stood a sloped
// face up to its highest corner from end to end -- 4042B's ledge at
// (-1140,-1397) runs 15.20 m to 15.61 m along its top, and a car running off
// it level with the road (15.3 m) met a 0.3 m lip its whole width and stopped
// dead, five cars in a row. An outline corner between the ends bends the
// band, and then both ends take all of it.
static void wallReach(const TopoPoly* p, float gx, float gy, float r2, float* x0,
	float* y0, float* x1, float* y1, float* lo0, float* hi0, float* lo1, float* hi1)
{
	const float ex = *x1 - *x0, ey = *y1 - *y0;
	const float e2 = ex * ex + ey * ey;
	if (e2 < 1e-6f)
		return;
	const float wx = *x0 - gx, wy = *y0 - gy;
	const float b = ex * wx + ey * wy, c = wx * wx + wy * wy - r2;
	const float disc = b * b - e2 * c;
	if (disc < 0.0f)
		return;
	const float r = sqrtf(disc);
	float ua = (-b - r) / e2, ub = (-b + r) / e2;
	ua = ua < 0.0f ? 0.0f : (ua > 1.0f ? 1.0f : ua);
	ub = ub < 0.0f ? 0.0f : (ub > 1.0f ? 1.0f : ub);
	if (ub < ua)
		return;
	float a0, b0, a1, b1;
	if (!faceBandAt(p, *x0, *y0, ex, ey, e2, ua, &a0, &b0) ||
	    !faceBandAt(p, *x0, *y0, ex, ey, e2, ub, &a1, &b1))
		return;
	const float eps = FACE_EDGE_M / sqrtf(e2);
	for (int k = 0; k < pnOf(p); k++) {
		const float uk = ((pxOf(p, k) - *x0) * ex + (pyOf(p, k) - *y0) * ey) / e2;
		if (uk <= ua + eps || uk >= ub - eps)
			continue;
		const float z = pzOf(p, k);
		const float lo = (a0 < a1 ? a0 : a1) < z ? (a0 < a1 ? a0 : a1) : z;
		const float hi = (b0 > b1 ? b0 : b1) > z ? (b0 > b1 ? b0 : b1) : z;
		a0 = a1 = lo;
		b0 = b1 = hi;
	}
	const float ox = *x0, oy = *y0;
	*x0 = ox + ex * ua;
	*y0 = oy + ey * ua;
	*x1 = ox + ex * ub;
	*y1 = oy + ey * ub;
	*lo0 = a0; *hi0 = b0;
	*lo1 = a1; *hi1 = b1;
}

// One query's state, so the merged grid and a tree can both feed it: the walls
// for topo_walls_near(), or every polygon for topo_track_polys(). The walk
// covers the box (bx0,by0)-(bx1,by1) and hands each entry to `offer`.
typedef struct WallGather WallGather;
struct WallGather {
	float x, y, radius, r2, zLo, zHi;
	float bx0, by0, bx1, by1;
	void (*offer)(Topology* t, WallGather* g, u32 idx);
	Model3DWall* out;
	u32* ids;            // topo_track_polys' list, in place of `out`
	int count, max;
	int worst;           // the farthest entry held once the buffer is full
	float worstD;
};

static void findWorst(WallGather* g)
{
	g->worst = -1;
	g->worstD = -1.0f;
	for (int i = 0; i < g->count; i++)
		if (g->out[i].dist2 > g->worstD) {
			g->worstD = g->out[i].dist2;
			g->worst = i;
		}
}

static void wallOffer(Topology* t, WallGather* g, u32 idx)
{
	const TopoPoly* p = &t->polys[idx];
	if (!(p->kind & TK_WALL))
		return;
	// BuildTrackPolygons (0x801f2d40) drops a polygon whose group is off, so
	// only the barriers of the race being run stop the car (TASKS.md #232).
	if (!t->groupOn[p->group])
		return;
	if (t->stamp[idx] == t->gen)
		return;
	t->stamp[idx] = t->gen;

	float bx0, by0, bx1, by1;
	polyBox(p, &bx0, &by0, &bx1, &by1);
	if (g->x + g->radius < bx0 || g->x - g->radius > bx1 ||
	    g->y + g->radius < by0 || g->y - g->radius > by1)
		return;

	float lo, hi;
	wallBand(p, &lo, &hi);
	if (hi < g->zLo || lo > g->zHi)
		return;

	float sx0, sy0, sx1, sy1;
	wallSegment(p, &sx0, &sy0, &sx1, &sy1);
	const float d2 = segDist2(sx0, sy0, sx1, sy1, g->x, g->y);
	if (d2 > g->r2)
		return;
	float lo0 = lo, hi0 = hi, lo1 = lo, hi1 = hi;
	wallReach(p, g->x, g->y, g->r2, &sx0, &sy0, &sx1, &sy1, &lo0, &hi0, &lo1, &hi1);
	lo = lo0 < lo1 ? lo0 : lo1;
	hi = hi0 > hi1 ? hi0 : hi1;
	if (hi < g->zLo || lo > g->zHi)
		return;

	Model3DWall* w;
	if (g->count < g->max) {
		w = &g->out[g->count++];
	} else {
		if (d2 >= g->worstD)
			return;
		w = &g->out[g->worst];
	}
	w->x0 = sx0;
	w->y0 = sy0;
	w->x1 = sx1;
	w->y1 = sy1;
	w->zmin = lo;
	w->zmax = hi;
	w->dist2 = d2;
	w->z0min = lo0;
	w->z0max = hi0;
	w->z1min = lo1;
	w->z1max = hi1;
	if (g->count >= g->max)
		findWorst(g);
}

// TrackPolygon gathering (TASKS.md #223): BuildTrackPolygons (0x801f2d40)
// takes every polygon whose box meets the query's, edges included, and whose
// group is on, and stops taking at 160. The visited flag it sets (0x20) is
// the stamp here.
static void polyOffer(Topology* t, WallGather* g, u32 idx)
{
	if (t->stamp[idx] == t->gen)
		return;
	t->stamp[idx] = t->gen;
	const TopoPoly* p = &t->polys[idx];
	float bx0, by0, bx1, by1;
	polyBox(p, &bx0, &by0, &bx1, &by1);
	if (bx1 < g->bx0 || bx0 > g->bx1 || by1 < g->by0 || by0 > g->by1)
		return;
	if (!t->groupOn[p->group] || g->count >= g->max)
		return;
	g->ids[g->count++] = idx;
}

static void mergedWalls(Topology* t, WallGather* g)
{
	int cx0 = (int)floorf((g->bx0 - t->ox) * t->invCell);
	int cx1 = (int)floorf((g->bx1 - t->ox) * t->invCell);
	int cy0 = (int)floorf((g->by0 - t->oy) * t->invCell);
	int cy1 = (int)floorf((g->by1 - t->oy) * t->invCell);
	if (cx0 < 0) cx0 = 0;
	if (cy0 < 0) cy0 = 0;
	if (cx1 >= t->gw) cx1 = t->gw - 1;
	if (cy1 >= t->gh) cy1 = t->gh - 1;
	for (int cy = cy0; cy <= cy1; cy++) {
		for (int cx = cx0; cx <= cx1; cx++) {
			const int c = cy * t->gw + cx;
			const u32 first = t->cells[c * 2];
			const u32 n = t->cells[c * 2 + 1];
			for (u32 e = 0; e < n; e++)
				g->offer(t, g, t->entries[first + e]);
		}
	}
}

// BuildTrackPolygons (0x801f2d40)'s walk of one tree over the query's box:
// every cell the box touches, truncated as retail truncates and clamped to
// the grid, and in each only the entries whose sub-cell mask meets the box's.
// The mask is per cell, worked out from the cell's own unclamped origin.
static void treeWalls(Topology* t, const TopoTree* tr, WallGather* g)
{
	const float x0 = g->bx0, y0 = g->by0;
	const float x1 = g->bx1, y1 = g->by1;
	const int cx0 = (int)((x0 - tr->ox) * TREE_CELL_INV);
	const int cx1 = (int)((x1 - tr->ox) * TREE_CELL_INV);
	const int cy0 = (int)((y0 - tr->oy) * TREE_CELL_INV);
	const int cy1 = (int)((y1 - tr->oy) * TREE_CELL_INV);
	const int nx = (int)((x1 - x0 + 8.0f) * TREE_SUB_INV) + 1;
	const int ny = (int)((y1 - y0 + 8.0f) * TREE_SUB_INV) + 1;
	const u32 runX = (nx >= 32) ? 0xFFFFFFFFu : (1u << nx) - 1;
	const u32 runY = (ny >= 32) ? 0xFFFFFFFFu : (1u << ny) - 1;

	for (int cy = cy0; cy <= cy1; cy++) {
		int iy = (int)((y0 - ((float)cy * TREE_CELL + tr->oy)) * TREE_SUB_INV);
		if (iy < 0)
			iy = 0;
		const u32 rows = (iy >= 32) ? 0 : runY << iy;
		const int ccy = cy < 0 ? 0 : (cy > tr->h - 1 ? tr->h - 1 : cy);
		for (int cx = cx0; cx <= cx1; cx++) {
			int ix = (int)((x0 - ((float)cx * TREE_CELL + tr->ox)) * TREE_SUB_INV);
			if (ix < 0)
				ix = 0;
			const u32 cols = ((ix >= 32) ? 0 : runX << ix) & 0xF;
			u32 mask = 0;
			if (rows & 1) mask |= cols;
			if (rows & 2) mask |= cols << 4;
			if (rows & 4) mask |= cols << 8;
			if (rows & 8) mask |= cols << 12;
			if (!mask)
				continue;
			const int ccx = cx < 0 ? 0 : (cx > tr->w - 1 ? tr->w - 1 : cx);
			const u16* cell = t->tcells + (tr->firstCell + (u32)(ccy * tr->w + ccx)) * 2;
			const u16* ent = t->tentries + (tr->firstEntry + cell[0]) * 2;
			for (u32 e = 0; e < cell[1]; e++)
				if (ent[e * 2 + 1] & mask)
					g->offer(t, g, tr->polyBase + ent[e * 2]);
		}
	}
}

// A fresh generation per query. Wrapping means every stamp could be stale,
// so the table is cleared then -- once every 255 queries, which at a few
// queries a frame is a few times a second and costs 150 KB of memset.
static void newGeneration(Topology* t)
{
	if (++t->gen == 0) {
		memset(t->stamp, 0, t->polyCount);
		t->gen = 1;
	}
}

int topo_walls_near(Topology* t, int coord, float x, float y, float radius,
	float zLo, float zHi, Model3DWall* out, int count, int max)
{
	if (!t || max <= 0 || radius <= 0.0f)
		return count;

	newGeneration(t);

	WallGather g;
	g.x = x;
	g.y = y;
	g.radius = radius;
	g.r2 = radius * radius;
	g.zLo = zLo;
	g.zHi = zHi;
	g.bx0 = x - radius;
	g.by0 = y - radius;
	g.bx1 = x + radius;
	g.by1 = y + radius;
	g.offer = wallOffer;
	g.ids = NULL;
	g.out = out;
	g.count = count;
	g.max = max;
	g.worst = -1;
	g.worstD = -1.0f;
	if (count >= max)
		findWorst(&g);

	// SetInterestBBox (0x801f1198) places the coordinate at the box's middle
	// with no margin, and the walls come from that tree alone.
	const int tree = coordTree(t, coord, x, y, INTEREST_RADIUS);
	if (tree >= 0)
		treeWalls(t, &t->trees[tree], &g);
	else
		mergedWalls(t, &g);
	return g.count;
}

int topo_track_polys(Topology* t, int coord, const float box[4], u32* ids, float* boxes,
	int max)
{
	if (!t || max <= 0)
		return 0;
	if (max > TOPO_TRACK_POLYS_MAX)
		max = TOPO_TRACK_POLYS_MAX;

	newGeneration(t);

	u32 found[TOPO_TRACK_POLYS_MAX];
	WallGather g;
	memset(&g, 0, sizeof(g));
	g.bx0 = box[0];
	g.by0 = box[1];
	g.bx1 = box[2];
	g.by1 = box[3];
	g.offer = polyOffer;
	g.ids = found;
	g.max = max;

	// SetInterestBBox places the coordinate at the box's middle with no margin
	// (radius 0x8033607c). With no tree there retail has no polygons at all;
	// the port reads the merged grid, as the walls do (PORT).
	const int tree = coordTree(t, coord, (box[0] + box[2]) * 0.5f,
		(box[1] + box[3]) * 0.5f, INTEREST_RADIUS);
	if (tree >= 0)
		treeWalls(t, &t->trees[tree], &g);
	else
		mergedWalls(t, &g);

	// BuildTrackPolygons' three passes over what it found: the roads, then
	// the walls, then the 0x08 surfaces that are not walls.
	int n = 0;
	for (int pass = 0; pass < 3; pass++)
		for (int i = 0; i < g.count; i++) {
			const u8 f = t->polys[found[i]].rflags;
			const int cls = (f & 0x02) ? 1 : ((f & 0x08) ? 2 : 0);
			if (cls != pass)
				continue;
			ids[n] = found[i];
			if (boxes)
				polyBox(&t->polys[found[i]], &boxes[n * 4], &boxes[n * 4 + 1],
					&boxes[n * 4 + 2], &boxes[n * 4 + 3]);
			n++;
		}
	return n;
}

bool topo_track_poly(const Topology* t, u32 id, TrackPoly* out)
{
	if (!t || id >= t->polyCount)
		return false;
	const TopoPoly* p = &t->polys[id];
	for (int k = 0; k < 4; k++) {
		out->v[k][0] = pxOf(p, k);
		out->v[k][1] = pyOf(p, k);
		out->v[k][2] = pzOf(p, k);
	}
	float e1[3], e2[3];
	for (int k = 0; k < 3; k++) {
		e1[k] = out->v[1][k] - out->v[0][k];
		e2[k] = out->v[2][k] - out->v[0][k];
	}
	const float nx = e1[1] * e2[2] - e1[2] * e2[1];
	const float ny = e1[2] * e2[0] - e1[0] * e2[2];
	const float nz = e1[0] * e2[1] - e1[1] * e2[0];
	const float len = sqrtf(nx * nx + ny * ny + nz * nz);
	if (len < 1e-9f)
		return false;
	out->n[0] = nx / len;
	out->n[1] = ny / len;
	out->n[2] = nz / len;
	out->d = out->v[0][0] * out->n[0] + out->v[0][1] * out->n[1] + out->v[0][2] * out->n[2];
	polyBox(p, &out->box[0], &out->box[1], &out->box[2], &out->box[3]);
	out->nv = pnOf(p);
	out->flags = p->rflags;
	out->group = p->group;
	out->terrain = p->terrain;
	return true;
}
