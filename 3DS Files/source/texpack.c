#include "texpack.h"
#include "linarena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTAINER_TAG   0xB3300000u
#define INFO_TAG        0xB3310000u
#define PIXEL_TAG       0xB3320000u
#define NAME_TAG        0x33310001u
#define INFO_RECORD_TAG 0x33310004u
#define FORMAT_RECORD_TAG 0x33310005u
#define PIXEL_LEAF_TAG  0x33320002u

// The boundary a pixel leaf's 0x11 filler run aligns its payload to. See the
// use in parse_container(); mirrored by PIXEL_LEAF_ALIGN in
// tools/extract_global_textures.py.
#define PIXEL_LEAF_ALIGN 16u

#define INFO_RECORD_SIZE 0x7C
#define FORMAT_RECORD_SIZE 0x20

#define TEXPACK_MAX_ENTRIES 512

// See texpack.h -- diagnostic only, never read by the loaders themselves.
TexPackFail g_texpackFail;

static void fail(int stage, u32 size, int count) {
	g_texpackFail.stage = stage;
	g_texpackFail.size = size;
	g_texpackFail.count = count;
}

static u32 ru32(const u8* b, size_t off) {
	u32 v;
	memcpy(&v, b + off, 4);
	return v;
}

static u16 ru16(const u8* b, size_t off) {
	u16 v;
	memcpy(&v, b + off, 2);
	return v;
}

typedef struct {
	u32 poffInLeaf; // offset within the pixel leaf (for length recovery, then discarded)
	int entryIdx;
} ActiveTexSort;

static int cmp_active(const void* a, const void* b) {
	const ActiveTexSort* ea = (const ActiveTexSort*)a;
	const ActiveTexSort* eb = (const ActiveTexSort*)b;
	if (ea->poffInLeaf < eb->poffInLeaf) return -1;
	if (ea->poffInLeaf > eb->poffInLeaf) return 1;
	return 0;
}

// build_entries()'s first-pass scratch. Deliberately NOT locals: as locals
// these four arrays made an 8776-byte stack frame, and the loader is reached
// two different ways -- from main() at startup (~12KB deep) and from
// loadFngScreen(), whose own frame is 25868 bytes. On the 16KB stack this
// project used to ask for, the first fit and the second ran off the bottom of
// the stack. It did not fault; parse_container() simply returned with
// corrupted locals and reported zero textures, so every .fng screen rendered
// untextured while the identical call at startup scanned all 211 textures.
// The stack is 256KB now (meta/app.rsf + the Makefile's __stacksize__), but a
// loader that silently mis-parses depending on how deep it is called from is a
// trap, so the scratch lives here instead and the frame no longer scales with
// TEXPACK_MAX_ENTRIES.
//
// Safe as file-scope state because build_entries() is called once per
// container, in a loop and never nested, and every texpack entry point
// is main-thread-only (the audio and VP6 workers never touch it). A threaded
// caller would have to give these back to the stack or lock them.
static ActiveTexSort s_active[TEXPACK_MAX_ENTRIES];
static u32           s_tmpHash[TEXPACK_MAX_ENTRIES];
static u16           s_tmpW[TEXPACK_MAX_ENTRIES], s_tmpH[TEXPACK_MAX_ENTRIES];
static TexPixFormat  s_tmpFmt[TEXPACK_MAX_ENTRIES];
static u32           s_tmpPalOff[TEXPACK_MAX_ENTRIES], s_tmpPalSize[TEXPACK_MAX_ENTRIES];

// The info chunk's two record tables, as offsets into whatever bytes were
// searched for them.
typedef struct {
	size_t infoOff, fmtOff;
	u32 nRecords;
	bool haveInfo, haveFmt;
} InfoTables;

static void find_info_tables(const u8* b, size_t start, size_t end, InfoTables* t) {
	size_t ip = start;
	while (ip + 8 <= end) {
		u32 itag = ru32(b, ip);
		u32 isize = ru32(b, ip + 4);
		size_t ipayload = ip + 8;
		if (ipayload + isize > end)
			break;
		if (itag == INFO_RECORD_TAG) {
			t->infoOff = ipayload;
			t->nRecords = isize / INFO_RECORD_SIZE;
			t->haveInfo = true;
		} else if (itag == FORMAT_RECORD_TAG) {
			t->fmtOff = ipayload;
			t->haveFmt = true;
		}
		ip = ipayload + isize;
	}
}

// The leaf payload does NOT start with pixel data: an 8-byte chunk header
// leaves it at 8 mod 16, so the packer inserts a run of 0x11 filler bytes to
// re-align the DXT block grid, and every info record's pixel offset is
// relative to the position AFTER that filler. It is 120 bytes in every one of
// the game's 1421 pixel leaves, each landing 16-byte aligned. Skipping it is
// not cosmetic: being 8 bytes off the block grid decodes standard DXT3 as
// coherent-but-horizontally-wrapped images (and makes the [alpha][colour]
// halves look swapped), which is how every DXT3 texture in this port used to
// render.
//
// The run length alone cannot be trusted to find the end of it, for exactly
// the reason TASKS.md #139 documents for the geometry leaves: a *real* leading
// pixel byte of 0x11 is indistinguishable from one more filler byte, so the
// scan walks straight past it. One leaf in the game does this --
// Global\InGameTextures.tpk in GLOBAL/InGameCommon.bun, whose first three
// pixel bytes are 11 11 11 -- and the 3-byte overrun shifted its whole block
// grid, rendering its neon, smoke and light-beam art as coloured scanline
// static (TASKS.md #148).
//
// The filler exists to align, so the position after it is
// PIXEL_LEAF_ALIGN-aligned. Backing the run off to the last aligned position
// inside it can therefore only ever shorten an overrun: a leaf whose genuine
// filler already ends aligned is left exactly as it was. Mirrors the same
// correction in tools/extract_global_textures.py.
//
// `lead` is the first `n` bytes of the leaf payload, which sits at offset `at`
// of the buffer or file; returns the filler's length.
static size_t leaf_filler(const u8* lead, size_t n, size_t at) {
	size_t pad = 0;
	while (pad < n && lead[pad] == 0x11)
		pad++;
	size_t back = (at + pad) % PIXEL_LEAF_ALIGN;
	if (back <= pad)
		pad -= back;
	return pad;
}

// Where one container's records and pixels are. In memory
// (texpack_load_mem()) `data` is the whole buffer and every offset indexes it.
// File-backed (texpack_load()) `data` is NULL, `rec` is a copy of the info
// chunk the table offsets index, and the leaf offset is a file offset.
typedef struct {
	const u8* rec;
	InfoTables t;
	size_t leafOff; // first pixel byte, after the filler
	u32 leafSize;
	size_t limit;   // bytes in the buffer or the file
	const u8* data;
	FILE* fp;
} ContainerView;

// Turns one container's records into out->entries[], starting at *outCount,
// up to TEXPACK_MAX_ENTRIES. Mirrors tools/extract_global_textures.py's
// parse_container().
static void build_entries(const ContainerView* v, TexPackEntry* entries, int* outCount) {
	const u8* rec = v->rec;
	const InfoTables* t = &v->t;

	// First pass: gather metadata for every record with primary pixel data.
	int nActive = 0;

	u32 palCursor = 0;

	for (u32 i = 0; i < t->nRecords && nActive < TEXPACK_MAX_ENTRIES; i++) {
		size_t roff = t->infoOff + (size_t)i * INFO_RECORD_SIZE;

		// A container's CLUTs are packed together at the HEAD of the pixel
		// leaf, one per palettized record in record order, each as long as
		// that record's +0x3C field says (0x400 = 256 BGRA entries on every
		// palettized record seen; 0 on every DXT/raw one). Nothing in the
		// record points at its own palette -- the position is implied by the
		// order, so the cursor has to walk EVERY record, including the ones
		// skipped just below, or a container that skips one lands every later
		// texture on the wrong CLUT.
		//
		// This used to read `pixelLeafOff` for every palettized texture, which
		// is only right when a container holds exactly one of them. L4RA's Y0
		// library holds 21 (the road, kerb and sidewalk art), and they would
		// all have drawn through the first one's palette.
		u32 paletteSize = ru32(rec, roff + 0x3C);
		u32 palOff = 0;
		if (t->haveFmt && paletteSize &&
		    ru32(rec, t->fmtOff + (size_t)i * FORMAT_RECORD_SIZE + 0x14) == 0x29) {
			palOff = palCursor;
			palCursor += paletteSize;
		}

		bool hasPrimary = ru32(rec, roff + 0x38) != 0;
		if (!hasPrimary)
			continue;
		u32 poffInLeaf = ru32(rec, roff + 0x30);
		u32 hash = ru32(rec, roff + 0x24);
		u16 width = ru16(rec, roff + 0x44);
		u16 height = ru16(rec, roff + 0x46);
		if (width == 0 || height == 0)
			continue;

		TexPixFormat fmt = TEXFMT_DXT1; // default: ASCII "DXT1" fourcc, or no format record at all
		if (t->haveFmt) {
			size_t foff = t->fmtOff + (size_t)i * FORMAT_RECORD_SIZE;
			u32 code = ru32(rec, foff + 0x14);
			if (memcmp(rec + foff + 0x14, "DXT1", 4) == 0)
				fmt = TEXFMT_DXT1;
			else if (memcmp(rec + foff + 0x14, "DXT3", 4) == 0)
				fmt = TEXFMT_DXT3;
			else if (code == 0x15)
				fmt = TEXFMT_RAW_BGRA8888;
			else if (code == 0x29 && paletteSize)
				fmt = TEXFMT_P8; // CLUT located by the record-order walk above
			else
				// Neither an ASCII DXT fourcc nor a format code we have
				// confirmed. Skip rather than guess -- same "hold on black"
				// rule as other unresolved widgets.
				continue;
		}

		s_active[nActive].poffInLeaf = poffInLeaf;
		s_active[nActive].entryIdx = nActive;
		s_tmpHash[nActive] = hash;
		s_tmpW[nActive] = width;
		s_tmpH[nActive] = height;
		s_tmpFmt[nActive] = fmt;
		s_tmpPalOff[nActive] = palOff;
		s_tmpPalSize[nActive] = paletteSize;
		nActive++;
	}

	qsort(s_active, nActive, sizeof(ActiveTexSort), cmp_active);

	// The CLUT block sits in front of the first texture's pixels. If the walk
	// ran past that, this container does not lay its palettes out the way the
	// ones we have decoded do, and picking a wrong CLUT would fabricate colour
	// -- so fall back to no palette, which drops those textures rather than
	// drawing them in invented colours.
	u32 firstPixel = nActive ? s_active[0].poffInLeaf : 0;

	for (int i = 0; i < nActive && *outCount < TEXPACK_MAX_ENTRIES; i++) {
		int idx = s_active[i].entryIdx;
		u32 poff = s_active[i].poffInLeaf;
		u32 length = (i + 1 < nActive) ? (s_active[i + 1].poffInLeaf - poff) : (v->leafSize - poff);

		u32 expected;
		if (s_tmpFmt[idx] == TEXFMT_RAW_BGRA8888) {
			expected = (u32)s_tmpW[idx] * s_tmpH[idx] * 4u;
		} else if (s_tmpFmt[idx] == TEXFMT_P8) {
			expected = (u32)s_tmpW[idx] * s_tmpH[idx]; // 1 byte/px index
		} else {
			u32 blocksW = (s_tmpW[idx] + 3) / 4;
			u32 blocksH = (s_tmpH[idx] + 3) / 4;
			expected = blocksW * blocksH * (s_tmpFmt[idx] == TEXFMT_DXT3 ? 16u : 8u);
		}
		if (expected > 0 && length > expected)
			length = expected; // trailing gap can include unrelated padding (mip levels, next texture)

		if (v->leafOff + poff + length > v->limit)
			continue;

		const bool hasPal = s_tmpFmt[idx] == TEXFMT_P8 && s_tmpPalSize[idx] &&
			s_tmpPalOff[idx] + s_tmpPalSize[idx] <= firstPixel &&
			v->leafOff + s_tmpPalOff[idx] + s_tmpPalSize[idx] <= v->limit;
		TexPackEntry* e = &entries[*outCount];
		memset(e, 0, sizeof(*e));
		e->hash = s_tmpHash[idx];
		e->width = s_tmpW[idx];
		e->height = s_tmpH[idx];
		e->format = s_tmpFmt[idx];
		e->pixelLen = length;
		if (v->data) {
			e->pixelData = v->data + v->leafOff + poff;
			e->palette = hasPal ? v->data + v->leafOff + s_tmpPalOff[idx] : NULL;
		} else {
			e->src = v->fp;
			e->srcOff = (u32)(v->leafOff + poff);
			e->srcPalOff = hasPal ? (u32)(v->leafOff + s_tmpPalOff[idx]) : 0;
		}
		(*outCount)++;
	}
}

// Parses one in-memory 0xb3300000 container (texpack_load_mem()).
static void parse_container(const u8* data, size_t fileSize, size_t containerOff, u32 containerSize,
                             TexPackEntry* entries, int* outCount) {
	size_t end = containerOff + 8 + containerSize;
	if (end > fileSize)
		end = fileSize;

	ContainerView v;
	memset(&v, 0, sizeof(v));
	v.rec = data;
	v.data = data;
	v.limit = fileSize;
	bool haveLeaf = false;

	size_t p = containerOff + 8;
	while (p + 8 <= end) {
		u32 tag = ru32(data, p);
		u32 size = ru32(data, p + 4);
		size_t payloadStart = p + 8;
		if (payloadStart + size > end)
			break;

		if (tag == INFO_TAG) {
			find_info_tables(data, payloadStart, payloadStart + size, &v.t);
		} else if (tag == PIXEL_TAG) {
			size_t ip = payloadStart, iend = payloadStart + size;
			while (ip + 8 <= iend) {
				u32 itag = ru32(data, ip);
				u32 isize = ru32(data, ip + 4);
				size_t ipayload = ip + 8;
				if (ipayload + isize > iend)
					break;
				if (itag == PIXEL_LEAF_TAG) {
					size_t pad = leaf_filler(data + ipayload, isize, ipayload);
					v.leafOff = ipayload + pad;
					v.leafSize = isize - (u32)pad;
					haveLeaf = true;
				}
				ip = ipayload + isize;
			}
		}
		p = payloadStart + size;
	}

	if (v.t.haveInfo && haveLeaf)
		build_entries(&v, entries, outCount);
}

static bool read_at(FILE* fp, size_t off, void* dst, size_t n) {
	return fseek(fp, (long)off, SEEK_SET) == 0 && fread(dst, 1, n, fp) == n;
}

// Every info chunk the game ships is a few tens of KB (records, names and the
// hash table); anything past this is not one.
#define INFO_CHUNK_MAX (1u << 20)
// Enough of a leaf's head to see its whole filler run (120 bytes in all 1421).
#define LEAF_LEAD 512u

// parse_container() for texpack_load(): the same walk over chunk headers read
// with seeks, keeping only the info chunk in memory.
static void parse_container_file(FILE* fp, size_t fileSize, size_t containerOff, u32 containerSize,
                                  TexPackEntry* entries, int* outCount) {
	size_t end = containerOff + 8 + containerSize;
	if (end > fileSize)
		end = fileSize;

	ContainerView v;
	memset(&v, 0, sizeof(v));
	v.limit = fileSize;
	v.fp = fp;
	u8* info = NULL;
	bool haveLeaf = false;
	u8 h[8];

	size_t p = containerOff + 8;
	while (p + 8 <= end && read_at(fp, p, h, 8)) {
		u32 tag = ru32(h, 0);
		u32 size = ru32(h, 4);
		size_t payloadStart = p + 8;
		if (payloadStart + size > end)
			break;

		if (tag == INFO_TAG && size <= INFO_CHUNK_MAX) {
			free(info);
			memset(&v.t, 0, sizeof(v.t));
			info = (u8*)malloc(size);
			if (info && read_at(fp, payloadStart, info, size)) {
				v.rec = info;
				find_info_tables(info, 0, size, &v.t);
			}
		} else if (tag == PIXEL_TAG) {
			size_t ip = payloadStart, iend = payloadStart + size;
			while (ip + 8 <= iend && read_at(fp, ip, h, 8)) {
				u32 itag = ru32(h, 0);
				u32 isize = ru32(h, 4);
				size_t ipayload = ip + 8;
				if (ipayload + isize > iend)
					break;
				if (itag == PIXEL_LEAF_TAG) {
					u8 lead[LEAF_LEAD];
					size_t n = isize < LEAF_LEAD ? isize : LEAF_LEAD;
					if (read_at(fp, ipayload, lead, n)) {
						size_t pad = leaf_filler(lead, n, ipayload);
						v.leafOff = ipayload + pad;
						v.leafSize = isize - (u32)pad;
						haveLeaf = true;
					}
				}
				ip = ipayload + isize;
			}
		}
		p = payloadStart + size;
	}

	if (v.rec && v.t.haveInfo && haveLeaf)
		build_entries(&v, entries, outCount);
	free(info);
}

// The texture containers are chunks of the file's own chunk tree -- top-level
// in GLOBALB.BUN, the InGame*.bun and every TRACKMAP*.BIN -- so following the
// chunk headers reaches them without reading anything else.
static void walk_file(FILE* fp, size_t start, size_t end, int depth,
                      TexPackEntry* entries, int* count) {
	u8 h[8];
	size_t p = start;
	while (p + 8 <= end && *count < TEXPACK_MAX_ENTRIES && read_at(fp, p, h, 8)) {
		u32 id = ru32(h, 0);
		u32 size = ru32(h, 4);
		if (p + 8 + (size_t)size > end)
			break;
		if (id == CONTAINER_TAG)
			parse_container_file(fp, end, p, size, entries, count);
		else if ((id & 0x80000000u) && depth < 3)
			walk_file(fp, p + 8, p + 8 + size, depth + 1, entries, count);
		p += 8 + (size_t)size;
	}
}

// Container tags can appear anywhere in the buffer (GLOBALB.BUN embeds two,
// at non-zero offsets, alongside unrelated data; decompressed FrontB.lzc
// likewise) -- brute byte scan for the tag, matching
// tools/extract_global_textures.py's find_containers(). Returns the entry
// count written into `entries`.
static int scan_containers(const u8* buf, size_t size, TexPackEntry* entries) {
	int count = 0;
	static const u8 tagBytes[4] = {0x00, 0x00, 0x30, 0xB3}; // 0xB3300000, little-endian
	for (size_t pos = 0; pos + 8 <= size && count < TEXPACK_MAX_ENTRIES;) {
		const u8* found = (const u8*)memchr(buf + pos, tagBytes[0], size - pos - 7);
		if (!found)
			break;
		size_t cand = (size_t)(found - buf);
		if (cand + 8 <= size && memcmp(buf + cand, tagBytes, 4) == 0) {
			u32 csize = ru32(buf, cand + 4);
			if (cand + 8 + csize <= size)
				parse_container(buf, size, cand, csize, entries, &count);
			pos = cand + 4;
		} else {
			pos = cand + 1;
		}
	}
	return count;
}

bool texpack_load(const char* path, TexPack* out) {
	memset(out, 0, sizeof(*out));
	fail(TEXPACK_FAIL_NONE, 0, 0);

	FILE* f = fopen(path, "rb");
	if (!f) {
		fail(TEXPACK_FAIL_OPEN, 0, 0);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0) {
		fclose(f);
		fail(TEXPACK_FAIL_SIZE, 0, 0);
		return false;
	}

	TexPackEntry* entries = (TexPackEntry*)calloc(TEXPACK_MAX_ENTRIES, sizeof(TexPackEntry));
	if (!entries) {
		fclose(f);
		fail(TEXPACK_FAIL_CALLOC_ENTRIES,
		     (u32)(TEXPACK_MAX_ENTRIES * sizeof(TexPackEntry)), 0);
		return false;
	}

	int count = 0;
	walk_file(f, 0, (size_t)size, 0, entries, &count);
	if (count > 0) {
		out->fileSize = (size_t)size;
		out->entries = entries;
		out->count = count;
		out->fp = f;
		return true;
	}

	// Nothing on the chunk tree: brute-scan the whole file the way
	// texpack_load_mem() does.
	memset(entries, 0, TEXPACK_MAX_ENTRIES * sizeof(TexPackEntry));
	u8* buf = (u8*)malloc((size_t)size);
	if (!buf) {
		free(entries);
		fclose(f);
		fail(TEXPACK_FAIL_MALLOC_FILE, (u32)size, 0);
		return false;
	}
	if (!read_at(f, 0, buf, (size_t)size)) {
		free(buf);
		free(entries);
		fclose(f);
		fail(TEXPACK_FAIL_READ, (u32)size, 0);
		return false;
	}
	fclose(f);

	count = scan_containers(buf, (size_t)size, entries);
	if (count <= 0) {
		// Hand nothing back on failure. This used to store `buf` into
		// out->fileBuf and still return false, so the callers -- all of which
		// guard texpack_free() on the return value -- dropped the whole
		// ~8MB GLOBALB.BUN read on the floor. Every .fng screen that failed
		// here leaked another 8MB, the malloc arena climbed 1.6MB -> 9.4MB ->
		// 24.8MB across three screens, and by UI_Main.fng the largest servable
		// block was too small for FrontB.lzc's ~6MB JDLZ expansion, so the
		// main menu could not load at all.
		free(buf);
		free(entries);
		fail(TEXPACK_FAIL_NO_CONTAINERS, (u32)size, count);
		return false;
	}

	out->fileBuf = buf;
	out->fileSize = (size_t)size;
	out->entries = entries;
	out->count = count;
	return true;
}

bool texpack_load_mem(const u8* buf, size_t len, TexPack* out) {
	memset(out, 0, sizeof(*out));
	fail(TEXPACK_FAIL_NONE, 0, 0);
	if (!buf || len < 8) {
		fail(TEXPACK_FAIL_SIZE, (u32)len, 0);
		return false;
	}

	TexPackEntry* entries = (TexPackEntry*)calloc(TEXPACK_MAX_ENTRIES, sizeof(TexPackEntry));
	if (!entries) {
		fail(TEXPACK_FAIL_CALLOC_ENTRIES,
		     (u32)(TEXPACK_MAX_ENTRIES * sizeof(TexPackEntry)), 0);
		return false;
	}

	int count = scan_containers(buf, len, entries);
	if (count <= 0) {
		// Same contract as texpack_load(): a false return means the caller owns
		// nothing and will not call texpack_free(), so release the entry table
		// here. `buf` is the caller's and is deliberately left alone.
		free(entries);
		fail(TEXPACK_FAIL_NO_CONTAINERS, (u32)len, count);
		return false;
	}

	// fileBuf stays NULL: the caller owns `buf`, so texpack_free() must not
	// free it (free(NULL) below is a no-op).
	out->fileBuf = NULL;
	out->fileSize = len;
	out->entries = entries;
	out->count = count;
	return true;
}

void texpack_free(TexPack* t) {
	free(t->fileBuf);
	free(t->entries);
	if (t->fp)
		fclose(t->fp);
	memset(t, 0, sizeof(*t));
}

// See texpack_load_shared() in the header. Four is room for every pack a
// screen is built from (GLOBALB, InGameCommon, InGameFreeRoam) with one over;
// past that the last slot is recycled, which is safe because a screen only
// borrows its packs for the length of its load.
#define TEXPACK_SHARED_MAX 4
static struct {
	char*   path;
	TexPack pack;
} s_shared[TEXPACK_SHARED_MAX];
static int s_sharedCount;

bool texpack_load_shared(const char* path, TexPack* out) {
	for (int i = 0; i < s_sharedCount; i++)
		if (strcmp(s_shared[i].path, path) == 0) {
			*out = s_shared[i].pack;
			return true;
		}
	if (!texpack_load(path, out))
		return false;
	if (out->fileBuf)
		return true;   // read whole: the caller's, released by texpack_release()
	char* key = strdup(path);
	if (!key)
		return true;
	if (s_sharedCount == TEXPACK_SHARED_MAX) {
		s_sharedCount--;
		free(s_shared[s_sharedCount].path);
		texpack_free(&s_shared[s_sharedCount].pack);
	}
	s_shared[s_sharedCount].path = key;
	s_shared[s_sharedCount].pack = *out;
	s_sharedCount++;
	return true;
}

void texpack_release(TexPack* t) {
	for (int i = 0; i < s_sharedCount; i++)
		if (s_shared[i].pack.entries == t->entries) {
			memset(t, 0, sizeof(*t));
			return;
		}
	texpack_free(t);
}

const TexPackEntry* texpack_find(const TexPack* t, u32 hash) {
	for (int i = 0; i < t->count; i++) {
		if (t->entries[i].hash == hash)
			return &t->entries[i];
	}
	return NULL;
}

// ---- DXT1/DXT3 -> RGBA8888 decode (C port of tools/dxt_transcode.py) ----

static void unpack565(u16 c, u8* r, u8* g, u8* b) {
	u8 r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
	*r = (u8)((r5 << 3) | (r5 >> 2));
	*g = (u8)((g6 << 2) | (g6 >> 4));
	*b = (u8)((b5 << 3) | (b5 >> 2));
}

static void decode_dxt1_block(const u8* data, u8 out[16][4]) {
	u16 c0, c1;
	memcpy(&c0, data, 2);
	memcpy(&c1, data + 2, 2);
	u32 bits;
	memcpy(&bits, data + 4, 4);

	u8 r0, g0, b0, r1, g1, b1;
	unpack565(c0, &r0, &g0, &b0);
	unpack565(c1, &r1, &g1, &b1);

	u8 colors[4][4] = {
		{r0, g0, b0, 255},
		{r1, g1, b1, 255},
		{0, 0, 0, 255},
		{0, 0, 0, 0},
	};
	if (c0 > c1) {
		colors[2][0] = (u8)((2 * r0 + r1) / 3);
		colors[2][1] = (u8)((2 * g0 + g1) / 3);
		colors[2][2] = (u8)((2 * b0 + b1) / 3);
		colors[3][0] = (u8)((r0 + 2 * r1) / 3);
		colors[3][1] = (u8)((g0 + 2 * g1) / 3);
		colors[3][2] = (u8)((b0 + 2 * b1) / 3);
		colors[3][3] = 255;
	} else {
		colors[2][0] = (u8)((r0 + r1) / 2);
		colors[2][1] = (u8)((g0 + g1) / 2);
		colors[2][2] = (u8)((b0 + b1) / 2);
	}

	for (int i = 0; i < 16; i++) {
		int idx = (bits >> (2 * i)) & 3;
		memcpy(out[i], colors[idx], 4);
	}
}

// Standard DXT3: each 16-byte block is [8B explicit 4-bit alpha][8B DXT1
// colour]. Worth stating explicitly because getting here took a wrong turn:
// decoding these textures at the WRONG base offset (see the 0x11 alignment
// filler handled in parse_container()) lands 8 bytes -- exactly half a block --
// off the true block grid, which makes standard [alpha][colour] data look
// convincingly like [colour][alpha] and produces images that are coherent but
// horizontally wrapped. The block layout is not the thing to change; the base
// offset is.
static void decode_dxt3_block(const u8* data, u8 out[16][4]) {
	u64 alphaBits;
	memcpy(&alphaBits, data, 8);
	decode_dxt1_block(data + 8, out);
	for (int i = 0; i < 16; i++) {
		u8 a4 = (u8)((alphaBits >> (4 * i)) & 0xF);
		out[i][3] = (u8)((a4 << 4) | a4);
	}
}

// Format-record code 0x15 (see texpack.h's TEXFMT_RAW_BGRA8888 comment):
// pixel data is raw, uncompressed, packed BGRA8888 (not DXT-block-encoded).
// Reorders to the R,G,B,A byte order decode_dxt() produces, since that's
// what upload_rgba_tiled() expects.
static u8* decode_raw_bgra8888(const u8* data, u32 dataLen, u16 width, u16 height) {
	size_t need = (size_t)width * height * 4;
	if (dataLen < need)
		return NULL;

	u8* out = (u8*)malloc(need);
	if (!out)
		return NULL;

	for (size_t i = 0; i < need; i += 4) {
		out[i + 0] = data[i + 2]; // R
		out[i + 1] = data[i + 1]; // G
		out[i + 2] = data[i + 0]; // B
		out[i + 3] = data[i + 3]; // A
	}
	return out;
}

// Decodes into a freshly malloc'd width*height*4 RGBA8888 buffer (caller
// frees). Returns NULL on unsupported input.
static u8* decode_dxt(const u8* data, u32 dataLen, u16 width, u16 height, bool isDxt3) {
	u32 blocksW = (width + 3) / 4, blocksH = (height + 3) / 4;
	u32 blockSize = isDxt3 ? 16u : 8u;
	if ((u64)blocksW * blocksH * blockSize > dataLen)
		return NULL;

	u8* out = (u8*)malloc((size_t)width * height * 4);
	if (!out)
		return NULL;

	u32 off = 0;
	for (u32 by = 0; by < blocksH; by++) {
		for (u32 bx = 0; bx < blocksW; bx++) {
			u8 pixels[16][4];
			if (isDxt3)
				decode_dxt3_block(data + off, pixels);
			else
				decode_dxt1_block(data + off, pixels);
			off += blockSize;

			for (int py = 0; py < 4; py++) {
				u32 y = by * 4 + py;
				if (y >= height)
					continue;
				for (int px = 0; px < 4; px++) {
					u32 x = bx * 4 + px;
					if (x >= width)
						continue;
					memcpy(out + ((size_t)y * width + x) * 4, pixels[py * 4 + px], 4);
				}
			}
		}
	}
	return out;
}

// ---- GPU upload: same PICA200 8x8-tile Morton swizzle + [A,B,G,R] byte
// order as vp6_display.c's video textures (see that file's header comment
// for why -- confirmed on real hardware, not just Azahar). ----

static int next_pow2(int v) {
	int p = 1;
	while (p < v) p <<= 1;
	return p < 8 ? 8 : p;
}

// The tester's 2026-11-08 career run: a three-car race left the ordinary
// linear heap 6 KB free (the opponents take what the world arena does not),
// so every frontend screen that followed it -- the post-race menu's icons,
// the Sponsorships logos, a third of the results screen -- uploaded nothing
// and drew its frames empty. The world's arena (linarena.h) still had 11 MB
// in one piece. So a texture the ordinary heap will not hold goes into the
// arena's low end instead, untagged (it never moves) -- only while a world
// is open, only for as long as the screen holding it lives (the #113 pool
// is off in the world, so nothing parks it there).
static u32 s_arenaUploads, s_uploadFails;
static u32 s_rgb8Uploads, s_rgb8SavedKB;

u32 texpack_arena_uploads(void) { return s_arenaUploads; }
u32 texpack_upload_fails(void) { return s_uploadFails; }
void texpack_rgb8_stats(u32* uploads, u32* savedKB) {
	if (uploads) *uploads = s_rgb8Uploads;
	if (savedKB) *savedKB = s_rgb8SavedKB;
}

static bool tex_init_anywhere(C3D_Tex* tex, int w, int h, GPU_TEXCOLOR fmt) {
	if (C3D_TexInit(tex, (u16)w, (u16)h, fmt))
		return true;
	if (!arena_active()) {
		s_uploadFails++;
		return false;
	}
	const ArenaSide side = arena_route(ARENA_LOW);
	const u32 tag = arena_tag(0);
	const bool ok = C3D_TexInit(tex, (u16)w, (u16)h, fmt);
	arena_tag(tag);
	arena_route(side);
	if (ok)
		s_arenaUploads++;
	else
		s_uploadFails++;
	return ok;
}

static bool upload_rgba_tiled(const u8* rgba, u16 srcW, u16 srcH, C3D_Tex* tex, Tex3DS_SubTexture* subtex) {
	int texW = next_pow2(srcW);
	int texH = next_pow2(srcH);

	// #437: opaque art that fills its texture exactly goes up as RGB8, three
	// bytes a texel instead of four and the same picture bit for bit -- the
	// GPU samples an RGB8 texel's alpha as 1, which is what the 255 was. The
	// PC's frontend art is all power-of-two sized (no upload since boot was
	// padded, measured on Azahar), so this covers the full-screen backdrops,
	// Loading.fng's among them, which stays resident through the world.
	// Never a padded texture: its padding is transparent, which an RGB8
	// texture cannot hold, and the filter reads it at the art's edge.
	bool rgb8 = texW == srcW && texH == srcH;
	for (size_t i = 0, n = (size_t)srcW * srcH; rgb8 && i < n; i++)
		rgb8 = rgba[i * 4 + 3] == 255;
	const int bpp = rgb8 ? 3 : 4;
	size_t bufBytes = (size_t)texW * texH * bpp;

	// Swizzled straight into the texture's own linear block: a staging copy
	// beside it used to need the same size again, for the length of the
	// upload, from a heap that may have neither.
	if (!tex_init_anywhere(tex, texW, texH, rgb8 ? GPU_RGB8 : GPU_RGBA8))
		return false;
	if (rgb8) {
		s_rgb8Uploads++;
		s_rgb8SavedKB += (u32)((size_t)texW * texH) >> 10;
	}
	u8* tiled = (u8*)tex->data;
	memset(tiled, 0, bufBytes);

	int blocksX = texW >> 3;
	for (int y = 0; y < srcH; y++) {
		int tyBlock = y >> 3, ly = y & 7;
		for (int x = 0; x < srcW; x++) {
			int txBlock = x >> 3, lx = x & 7;
			int local = (lx & 1) | ((ly & 1) << 1) | (((lx >> 1) & 1) << 2) |
			            (((ly >> 1) & 1) << 3) | (((lx >> 2) & 1) << 4) | (((ly >> 2) & 1) << 5);
			const u8* src = rgba + ((size_t)y * srcW + x) * 4;
			u8* dst = tiled + ((size_t)(tyBlock * blocksX + txBlock) * 64 + local) * bpp;
			if (rgb8) {
				// The PICA's RGB8 texel is [B,G,R]: RGBA8's [A,B,G,R] without the A.
				dst[0] = src[2]; // B
				dst[1] = src[1]; // G
				dst[2] = src[0]; // R
				continue;
			}
			dst[0] = src[3]; // A
			dst[1] = src[2]; // B
			dst[2] = src[1]; // G
			dst[3] = src[0]; // R
		}
	}

	// Deliberately NOT mipmapped, unlike model3d.c and world.c. Two reasons,
	// and the first one is a correctness argument rather than a cost one:
	// every texture here is padded up to the next power of two and handed out
	// as a subtexture (`subtex` below covers only srcW x srcH of it), so
	// halving it would fold the padding into the used region's edge texels and
	// the subtexture rectangle would no longer land on a texel boundary. A
	// mip chain over a padded atlas is wrong, not just wasteful. Second, these
	// are frontend quads drawn at roughly 1:1 -- nothing here recedes into the
	// distance, which is the only thing minification filtering helps.
	C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

	C3D_TexFlush(tex);

	subtex->width = srcW;
	subtex->height = srcH;
	subtex->left = 0.0f;
	subtex->top = 1.0f;
	subtex->right = (float)srcW / (float)texW;
	subtex->bottom = 1.0f - (float)srcH / (float)texH;
	return true;
}

void texcache_init(TexCache* c) {
	memset(c, 0, sizeof(*c));
}

// ---- #113: the cross-screen pool -------------------------------------------
// A screen's cache used to be thrown away whole at every transition, so every
// navigation decoded and uploaded the same frontend chrome again. Now a freed
// screen's textures park here, and the next screen's texcache_get() takes a
// parked texture back instead of decoding it. Keyed by the hash and the pack
// entry's size and format, so a same-named texture from another pack with
// other pixels is never handed out in its place. Bounded (TEX_POOL_BYTES,
// least recently parked out first) and switched off while a world is open:
// world.c sizes its section pager from the free linear heap at world_open(),
// so main.c flushes the pool before that and keeps it off until the world
// closes.
#define TEX_POOL_MAX   192
#define TEX_POOL_BYTES (8u << 20)
// Linear heap a texture must leave free to be parked rather than deleted.
#define TEX_POOL_MARGIN (24u << 20)

typedef struct {
	TexCacheSlot s;
	u16 w, h;
	u32 fmt, len;
	u32 stamp;
} TexPoolSlot;

static TexPoolSlot s_pool[TEX_POOL_MAX];
static int s_poolCount;
static u32 s_poolBytes, s_poolClock;
static bool s_poolOn = true;
static u32 s_poolHits, s_poolMisses;

static void poolDrop(int i) {
	C3D_TexDelete(&s_pool[i].s.tex);
	s_poolBytes -= s_pool[i].s.tex.size;
	s_pool[i] = s_pool[--s_poolCount];
}

void texcache_pool_flush(void) {
	while (s_poolCount > 0)
		poolDrop(s_poolCount - 1);
	s_poolBytes = 0;
}

void texcache_pool_enable(bool on) {
	s_poolOn = on;
	if (!on)
		texcache_pool_flush();
}

void texcache_pool_stats(u32* hits, u32* misses, int* parked, u32* bytes) {
	if (hits) *hits = s_poolHits;
	if (misses) *misses = s_poolMisses;
	if (parked) *parked = s_poolCount;
	if (bytes) *bytes = s_poolBytes;
}

// Parks one slot of a cache being freed; false = the caller deletes it.
static bool poolPark(const TexCacheSlot* sl) {
	if (!s_poolOn || !sl->key.len)
		return false;
	const u32 sz = sl->tex.size;
	if (sz > TEX_POOL_BYTES)
		return false;
	for (int i = 0; i < s_poolCount; i++)
		if (s_pool[i].s.hash == sl->hash && s_pool[i].len == sl->key.len &&
				s_pool[i].w == sl->key.w && s_pool[i].h == sl->key.h &&
				s_pool[i].fmt == sl->key.fmt)
			return false; // already parked: this copy goes
	while (s_poolCount > 0 && (s_poolCount >= TEX_POOL_MAX || s_poolBytes + sz > TEX_POOL_BYTES)) {
		int old = 0;
		for (int i = 1; i < s_poolCount; i++)
			if (s_pool[i].stamp < s_pool[old].stamp)
				old = i;
		poolDrop(old);
	}
	if (linearSpaceFree() < TEX_POOL_MARGIN)
		return false;
	TexPoolSlot* p = &s_pool[s_poolCount++];
	p->s = *sl;
	p->w = sl->key.w;
	p->h = sl->key.h;
	p->fmt = sl->key.fmt;
	p->len = sl->key.len;
	p->stamp = ++s_poolClock;
	s_poolBytes += sz;
	return true;
}

void texcache_free(TexCache* c) {
	for (int i = 0; i < c->count; i++) {
		if (c->slots[i].valid && !poolPark(&c->slots[i]))
			C3D_TexDelete(&c->slots[i].tex);
	}
	memset(c, 0, sizeof(*c));
}

// NOTE (resolved): a fix_u2_loading_bg_wrap() hack used to live here, rolling
// U2_LOADING_BG (0x70eb2ccb) left by 85px to undo an unexplained horizontal
// wrap. The cause was the pixel-leaf base offset, now fixed in
// parse_container() by skipping the leaf's leading 0x11 alignment filler. With
// that corrected the texture decodes with no wrap, so the hack is gone rather
// than retained -- keeping it would corrupt a correct image.

// TEXFMT_P8: one index byte per pixel into the leaf's shared 256-entry CLUT.
// Palette entries are BGRA8888 (same channel order as decode_raw_bgra8888's
// input), reordered here to the R,G,B,A order decode_dxt() produces.
static u8* decode_p8(const u8* data, u32 len, const u8* palette, u16 width, u16 height) {
	u32 npix = (u32)width * height;
	if (!palette || len < npix)
		return NULL;
	u8* out = (u8*)malloc((size_t)npix * 4);
	if (!out)
		return NULL;
	for (u32 i = 0; i < npix; i++) {
		const u8* p = palette + (u32)data[i] * 4;
		out[i * 4 + 0] = p[2];
		out[i * 4 + 1] = p[1];
		out[i * 4 + 2] = p[0];
		out[i * 4 + 3] = p[3];
	}
	return out;
}

// The format dispatch shared by texcache_get() and texpack_decode_entry().
// Returns a malloc'd width*height RGBA8888 buffer in the R,G,B,A order
// upload_rgba_tiled() expects, or NULL.
static u8* decode_entry_rgba(const TexPackEntry* e) {
	if (!e || e->width == 0 || e->height == 0)
		return NULL;
	const u8* px = e->pixelData;
	const u8* pal = e->palette;
	u8* own = NULL;
	if (!px) {
		// A texpack_load() entry: its bytes are still on the card. A CLUT is
		// 256 BGRA entries, as decode_p8() reads it.
		const size_t palLen = e->srcPalOff ? 0x400u : 0u;
		own = e->src ? (u8*)malloc((size_t)e->pixelLen + palLen) : NULL;
		if (!own || !read_at(e->src, e->srcOff, own, e->pixelLen) ||
				(palLen && !read_at(e->src, e->srcPalOff, own + e->pixelLen, palLen))) {
			free(own);
			return NULL;
		}
		px = own;
		if (palLen)
			pal = own + e->pixelLen;
	}
	u8* rgba;
	if (e->format == TEXFMT_RAW_BGRA8888)
		rgba = decode_raw_bgra8888(px, e->pixelLen, e->width, e->height);
	else if (e->format == TEXFMT_P8)
		rgba = decode_p8(px, e->pixelLen, pal, e->width, e->height);
	else
		rgba = decode_dxt(px, e->pixelLen, e->width, e->height, e->format == TEXFMT_DXT3);
	free(own);
	return rgba;
}

// In-place 2:1 box filter, halving both axes. Only ever called on even
// dimensions (the callers halve powers of two), so there is no edge case to
// handle for an odd trailing row/column.
static void halve_rgba(u8* rgba, u16* w, u16* h) {
	u16 nw = (u16)(*w / 2), nh = (u16)(*h / 2);
	for (u16 y = 0; y < nh; y++) {
		for (u16 x = 0; x < nw; x++) {
			const u8* a = rgba + (((size_t)(y * 2) * *w) + x * 2) * 4;
			const u8* b = a + 4;
			const u8* c = a + (size_t)*w * 4;
			const u8* d = c + 4;
			u8* o = rgba + ((size_t)y * nw + x) * 4;
			for (int k = 0; k < 4; k++)
				o[k] = (u8)((a[k] + b[k] + c[k] + d[k] + 2) / 4);
		}
	}
	*w = nw;
	*h = nh;
}

bool texpack_decode_entry(const TexPackEntry* e, int maxDim, bool opaqueIfAlphaEmpty,
		C3D_Tex* tex, Tex3DS_SubTexture* subtex) {
	u8* rgba = decode_entry_rgba(e);
	if (!rgba)
		return false;
	u16 w = e->width, h = e->height;

	// See texpack.h. A texture whose every texel is alpha 0 renders as
	// literally nothing, so promoting it to opaque cannot destroy an image the
	// artist authored -- there is no image to destroy either way. Answered from the real decoded texels, and
	// only when the caller says its texture family is one where an empty alpha
	// block means "no alpha", never globally.
	if (opaqueIfAlphaEmpty) {
		size_t npix = (size_t)w * h;
		bool empty = true;
		for (size_t i = 0; i < npix; i++) {
			if (rgba[i * 4 + 3] != 0) {
				empty = false;
				break;
			}
		}
		if (empty) {
			for (size_t i = 0; i < npix; i++)
				rgba[i * 4 + 3] = 255;
		}
	}
	// Box-filter down while the texture is bigger than the caller's budget and
	// both axes are still even. See trackmap.h for why the track maps take
	// this: they ship at 512x512 (1MiB as RGBA8) but are only ever drawn into
	// a ~163px panel, so a half-size copy is above the displayed resolution
	// and costs a quarter of the linear memory.
	while (maxDim > 0 && ((int)w > maxDim || (int)h > maxDim) &&
			(w & 1) == 0 && (h & 1) == 0 && w > 1 && h > 1)
		halve_rgba(rgba, &w, &h);
	bool ok = upload_rgba_tiled(rgba, w, h, tex, subtex);
	free(rgba);
	return ok;
}

bool texpack_decode_entry_alpha_full(const TexPackEntry* e, C3D_Tex* tex,
		Tex3DS_SubTexture* subtex) {
	u8* rgba = decode_entry_rgba(e);
	if (!rgba)
		return false;
	const size_t npix = (size_t)e->width * e->height;
	u8 peak = 0;
	for (size_t i = 0; i < npix; i++) {
		if (rgba[i * 4 + 3] > peak)
			peak = rgba[i * 4 + 3];
	}
	// See texpack.h. Nothing to rescale when the art is already opaque
	// somewhere, or when it is empty (which would divide by zero).
	if (peak > 0 && peak < 255) {
		for (size_t i = 0; i < npix; i++) {
			const unsigned a = (unsigned)rgba[i * 4 + 3] * 255u / peak;
			rgba[i * 4 + 3] = (u8)(a > 255u ? 255u : a);
		}
	}
	const bool ok = upload_rgba_tiled(rgba, e->width, e->height, tex, subtex);
	free(rgba);
	return ok;
}

C2D_Image texcache_get(TexCache* c, const TexPack* pack, u32 hash) {
	C2D_Image zero = {0};
	for (int i = 0; i < c->count; i++) {
		if (c->slots[i].valid && c->slots[i].hash == hash)
			return c->slots[i].image;
	}
	if (c->count >= TEX_CACHE_MAX)
		return zero;

	const TexPackEntry* e = texpack_find(pack, hash);
	if (!e)
		return zero;

	TexCacheSlot* slot = &c->slots[c->count];
	// #113: parked by an earlier screen, the same pixels -- taken back whole.
	for (int i = 0; i < s_poolCount; i++) {
		const TexPoolSlot* p = &s_pool[i];
		if (p->s.hash != hash || p->w != e->width || p->h != e->height ||
				p->fmt != (u32)e->format || p->len != e->pixelLen)
			continue;
		*slot = p->s;
		s_poolBytes -= p->s.tex.size;
		s_pool[i] = s_pool[--s_poolCount];
		slot->image.tex = &slot->tex;
		slot->image.subtex = &slot->subtex;
		slot->valid = true;
		c->count++;
		s_poolHits++;
		return slot->image;
	}

	u8* rgba = decode_entry_rgba(e);
	if (!rgba)
		return zero;

	bool ok = upload_rgba_tiled(rgba, e->width, e->height, &slot->tex, &slot->subtex);
	free(rgba);
	if (!ok)
		return zero;
	s_poolMisses++;

	slot->key.w = e->width;
	slot->key.h = e->height;
	slot->key.fmt = (u32)e->format;
	slot->key.len = e->pixelLen;
	slot->hash = hash;
	slot->image.tex = &slot->tex;
	slot->image.subtex = &slot->subtex;
	slot->valid = true;
	c->count++;
	return slot->image;
}

C2D_Image texcache_find(const TexCache* c, u32 hash) {
	C2D_Image zero = {0};
	for (int i = 0; i < c->count; i++) {
		if (c->slots[i].valid && c->slots[i].hash == hash)
			return c->slots[i].image;
	}
	return zero;
}

// The port's own sheets (data/buttons3ds.bin) go up the same way a pack's
// textures do -- the 8x8-tile Morton swizzle above is the only one confirmed
// on hardware, so there is no second copy of it anywhere in the port.
u8* texpack_decode_rgba(const TexPackEntry* e, u16* w, u16* h) {
	u8* rgba = decode_entry_rgba(e);
	if (rgba) {
		if (w) *w = e->width;
		if (h) *h = e->height;
	}
	return rgba;
}

bool texpack_upload_rgba(const u8* rgba, u16 w, u16 h, C3D_Tex* tex,
		Tex3DS_SubTexture* subtex) {
	if (!rgba || !tex || !subtex || w == 0 || h == 0)
		return false;
	return upload_rgba_tiled(rgba, w, h, tex, subtex);
}
