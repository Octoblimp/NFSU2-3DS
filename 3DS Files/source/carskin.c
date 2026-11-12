// CompositeSkin + CompositeSkin32 (SPEED2.EXE 0x61D790 / 0x611F10), and the
// CompositeRim / CompositeSpinner it ends with (0x6343A0 / 0x61E040); see
// carskin.h. tools/vinyl_pack.py's compose() and tools/rim_pack.py's
// composite() are the same composites in numpy, the references these were
// checked against.
#include "carskin.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// tools/vinyl_pack.py's layout.
#define NFSV_MAGIC    0x5653464Eu  // 'NFSV' little-endian
#define NFSV_VERSION  1
#define NFSV_HDR      0x30
#define NFSV_TEX      20
#define NFSV_PART     12
#define SWATCH_MAX    16
#define NFSV_SWATCH   (4 + 4 * SWATCH_MAX)

enum { KIND_ARGB = 0, KIND_P8 = 1, KIND_G8 = 2 };

#define NO_HASH 0xFFFFFFFFu

// PORT: retail composites nothing for a car without BASE_PAINT. Such a car
// here wears the promo blue model3d.c's CAR_PAINT_COLOR has always been.
#define FALLBACK_PAINT 0x001C4696u
// The skin a card without the car's .nfsv gets: DUMMY_SKIN1's size.
#define FALLBACK_SIZE  512

// An empty remap colour slot k is 0xFF in byte k of retail's R|G<<8|B<<16|A<<24
// -- the image's own channel -- which is these once swapped to 0xAARRGGBB.
static const u32 kEmptyRemap[CARSKIN_REMAPS] = {
	0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u,
};

static u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 rd16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }

// bStringHash carried on from `h` (seed 0xFFFFFFFF for a plain hash). Every
// name built here is already upper case.
static u32 ext(u32 h, const char* s) {
	while (*s)
		h = h * 33u + (u8)*s++;
	return h;
}

// ------------------------------------------------------------------ file

typedef struct {
	FILE* f;
	u8* tab;    // the file's bytes [0, data offset)
	u32 w, h, nTex, nPart, oTex, oPart, oSwatch, oData;
} Nfsv;

static void nfsv_close(Nfsv* v) {
	if (v->f)
		fclose(v->f);
	free(v->tab);
	memset(v, 0, sizeof(*v));
}

static bool nfsv_open(Nfsv* v, const char* path) {
	memset(v, 0, sizeof(*v));
	v->f = fopen(path, "rb");
	if (!v->f)
		return false;
	u8 h[NFSV_HDR];
	bool ok = fread(h, 1, NFSV_HDR, v->f) == NFSV_HDR && rd32(h) == NFSV_MAGIC &&
	          rd32(h + 4) == NFSV_VERSION;
	if (ok) {
		v->w = rd16(h + 8);       v->h = rd16(h + 10);
		v->nTex = rd32(h + 12);   v->nPart = rd32(h + 16);
		v->oTex = rd32(h + 20);   v->oPart = rd32(h + 24);
		v->oSwatch = rd32(h + 28); v->oData = rd32(h + 32);
		ok = v->w && v->h && v->w <= 1024 && v->h <= 1024 &&
		     (u64)v->oSwatch + CARSKIN_REGIONS * NFSV_SWATCH <= v->oData &&
		     (u64)v->oTex + (u64)v->nTex * NFSV_TEX <= v->oData &&
		     (u64)v->oPart + (u64)v->nPart * NFSV_PART <= v->oData &&
		     v->oData <= (1u << 20);
	}
	if (ok) {
		v->tab = (u8*)malloc(v->oData);
		ok = v->tab && fseek(v->f, 0, SEEK_SET) == 0 &&
		     fread(v->tab, 1, v->oData, v->f) == v->oData;
	}
	if (!ok)
		nfsv_close(v);
	return ok;
}

// Both tables are sorted by their first u32.
static const u8* find(const u8* tab, u32 n, u32 stride, u32 hash) {
	u32 lo = 0, hi = n;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		const u8* e = tab + mid * stride;
		const u32 h = rd32(e);
		if (h == hash)
			return e;
		if (h < hash) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

static const u8* tex_find(const Nfsv* v, u32 hash) {
	return find(v->tab + v->oTex, v->nTex, NFSV_TEX, hash);
}

static const u8* part_find(const Nfsv* v, u32 hash) {
	return find(v->tab + v->oPart, v->nPart, NFSV_PART, hash);
}

// ------------------------------------------------------------------ cache
// #402: while held (carskin_cache_hold), the car's .nfsv stays open from one
// composite to the next and every texture inflated out of it is kept, so a
// vinyl previewed after another re-blends but reads and inflates only the new
// vinyl's art.

#define SC_BUDGET (4u << 20)

typedef struct {
	u32 off, size;
	u8* data;
} ScRun;

static struct {
	bool hold;
	char path[320];
	Nfsv v;           // f NULL when nothing is kept
	ScRun run[64];
	int nRun;
	u32 bytes;
} s_sc;

static void sc_free(void) {
	nfsv_close(&s_sc.v);
	for (int i = 0; i < s_sc.nRun; i++)
		free(s_sc.run[i].data);
	s_sc.nRun = 0;
	s_sc.bytes = 0;
	s_sc.path[0] = '\0';
}

void carskin_cache_hold(bool hold) {
	if (!hold && (s_sc.v.f || s_sc.nRun))
		sc_free();
	s_sc.hold = hold;
}

// nfsv_open() through the cache: `v` shares the kept file when it is `path`.
static bool skin_open(Nfsv* v, const char* path) {
	if (s_sc.hold && s_sc.v.f && strcmp(s_sc.path, path) == 0) {
		*v = s_sc.v;
		return true;
	}
	if (!nfsv_open(v, path))
		return false;
	if (s_sc.hold) {
		sc_free();
		s_sc.v = *v;
		snprintf(s_sc.path, sizeof(s_sc.path), "%s", path);
	}
	return true;
}

// And nfsv_close(): the kept file stays open.
static void skin_close(Nfsv* v) {
	if (s_sc.v.f && v->f == s_sc.v.f)
		memset(v, 0, sizeof(*v));
	else
		nfsv_close(v);
}

static bool inflate_raw(const Nfsv* v, const u8* e, void* dst, u32 size);

// A texture entry's zlib stream inflated into `dst`, which must come out at
// exactly `size` bytes; from RAM when the kept file's entry was inflated
// before.
static bool inflate_to(const Nfsv* v, const u8* e, void* dst, u32 size) {
	const bool kept = s_sc.hold && s_sc.v.f && v->f == s_sc.v.f;
	const u32 off = rd32(e + 12);
	if (kept)
		for (int i = 0; i < s_sc.nRun; i++)
			if (s_sc.run[i].off == off && s_sc.run[i].size == size) {
				memcpy(dst, s_sc.run[i].data, size);
				return true;
			}
	if (!inflate_raw(v, e, dst, size))
		return false;
	if (kept && s_sc.nRun < (int)(sizeof(s_sc.run) / sizeof(s_sc.run[0])) &&
			s_sc.bytes + size <= SC_BUDGET) {
		u8* c = (u8*)malloc(size);
		if (c) {
			memcpy(c, dst, size);
			s_sc.run[s_sc.nRun++] = (ScRun){ off, size, c };
			s_sc.bytes += size;
		}
	}
	return true;
}

static bool inflate_raw(const Nfsv* v, const u8* e, void* dst, u32 size) {
	const u32 off = rd32(e + 12), zsize = rd32(e + 16);
	u8* z = (u8*)malloc(zsize ? zsize : 1);
	bool ok = z && fseek(v->f, (long)(v->oData + off), SEEK_SET) == 0 &&
	          fread(z, 1, zsize, v->f) == zsize;
	uLongf got = size;
	ok = ok && uncompress((Bytef*)dst, &got, z, zsize) == Z_OK && got == size;
	free(z);
	return ok;
}

// ----------------------------------------------------------------- blend

// FUN_00611B00's products: an image channel c weights a remap colour's byte b
// by w(c) = c * (float)(1/255), and the byte it adds is trunc(b * w(c)), both
// products in single precision -- the x87 under D3D9's default FPU mode. A
// weight counts only when it is above 1/255 itself, so c = 1 adds nothing.
static u8 s_T[256][256];
static bool s_live[256];
static bool s_tReady;

static void build_table(void) {
	if (s_tReady)
		return;
	const float f = 1.0f / 255.0f;   // 0x3B808081, the constant at 0x784548
	for (int c = 0; c < 256; c++) {
		const float w = (float)c * f;
		s_live[c] = w > f;
		for (int b = 0; b < 256; b++)
			s_T[c][b] = (u8)(int)((float)b * w);
	}
	s_tReady = true;
}

// A remapped texel: its R, G and B are the weights of the first three remap
// colours. Each colour byte is the clamped sum of the weighted bytes; alpha is
// the largest of the weighted alphas instead.
static u32 remap_texel(u32 px, const u32 rc[CARSKIN_REMAPS]) {
	const u8 c[3] = { (u8)(px >> 16), (u8)(px >> 8), (u8)px };
	u32 out = 0, a = 0;
	for (int shift = 0; shift < 24; shift += 8) {
		u32 acc = 0;
		for (int k = 0; k < 3; k++)
			if (s_live[c[k]])
				acc += s_T[c[k]][(rc[k] >> shift) & 0xFF];
		out |= (acc > 255 ? 255 : acc) << shift;
	}
	for (int k = 0; k < 3; k++)
		if (s_live[c[k]] && s_T[c[k]][rc[k] >> 24] > a)
			a = s_T[c[k]][rc[k] >> 24];
	return out | (a << 24);
}

// A mask's green byte m under 0x80 lays the image over the skin at m/128,
// every byte (alpha included) as two truncated halves.
static u32 lerp_texel(u32 img, u32 skin, u32 m) {
	u32 out = 0;
	for (int shift = 0; shift < 32; shift += 8)
		out |= ((((img >> shift) & 0xFF) * m >> 7) +
		        (((skin >> shift) & 0xFF) * (128 - m) >> 7)) << shift;
	return out;
}

typedef struct {
	u32* img;   // n texels
	u8* mask;   // n
	u8* p8;     // 1024 + n: a P8 image's palette and indices
	u32 n;
} Scratch;

// One layer of CompositeSkin32: image `th` through `th`_MASK, remapped first
// when `remap` is given. False (and the skin untouched) when either is missing
// -- CompositeSkin drops such a layer.
static bool layer(const Nfsv* v, u32 th, const u32* remap, u32* skin, Scratch* s) {
	const u8* te = tex_find(v, th);
	const u8* me = tex_find(v, ext(th, "_MASK"));
	if (!te || !me || te[8] == KIND_G8 || me[8] != KIND_G8)
		return false;
	for (int i = 0; i < 2; i++) {
		const u8* e = i ? me : te;
		if (rd16(e + 4) != v->w || rd16(e + 6) != v->h)
			return false;
	}
	const u32 n = s->n;
	if (te[8] == KIND_ARGB) {
		// The D3D surface's own dwords, little-endian like this CPU.
		if (!inflate_to(v, te, s->img, n * 4))
			return false;
	} else if (te[8] == KIND_P8) {
		if (!inflate_to(v, te, s->p8, 1024 + n))
			return false;
		u32 pal[256];
		memcpy(pal, s->p8, sizeof(pal));
		const u8* idx = s->p8 + 1024;
		for (u32 i = 0; i < n; i++)
			s->img[i] = pal[idx[i]];
	} else {
		return false;
	}
	if (!inflate_to(v, me, s->mask, n))
		return false;

	for (u32 i = 0; i < n; i++) {
		const u32 m = s->mask[i];
		if (!m)
			continue;
		const u32 px = remap ? remap_texel(s->img[i], remap) : s->img[i];
		skin[i] = m >= 0x80 ? px : lerp_texel(px, skin[i], m);
	}
	return true;
}

// ------------------------------------------------------------------- api

static u32 colour(const PaintTable* t, u32 part, u32 fallback) {
	return t ? paint_lookup_argb(t, part, fallback) : fallback;
}

static void slog(void (*log)(const char*), const char* msg) {
	if (log)
		log(msg);
}

u32* carskin_compose(const char* dataRoot, const char* car, const CarSkinDesc* d,
		const PaintTable* paint, int* w, int* h, void (*log)(const char* msg)) {
	const u64 t0 = osGetTime();
	char b[224];

	// CompositeSkin's colours: R | G<<8 | B<<16 | GLOSS<<24 off each part,
	// swapped to 0xAARRGGBB (FUN_005BE040) -- paint_lookup_argb() as it is.
	const u32 base = d->basePaint ? colour(paint, d->basePaint, FALLBACK_PAINT)
	                              : FALLBACK_PAINT;
	u32 region[CARSKIN_REGIONS];
	for (int i = 0; i < CARSKIN_REGIONS; i++)
		region[i] = d->regionPart[i] ? colour(paint, d->regionPart[i], base) : base;

	char path[320];
	snprintf(path, sizeof(path), "%s/CARS/%s.nfsv", dataRoot, car);
	Nfsv v;
	const bool have = skin_open(&v, path);
	const u32 sw = have ? v.w : FALLBACK_SIZE, sh = have ? v.h : FALLBACK_SIZE;
	const u32 n = sw * sh;
	u32* skin = (u32*)malloc(n * 4);
	if (!skin) {
		skin_close(&v);
		return NULL;
	}
	for (u32 i = 0; i < n; i++)
		skin[i] = base;
	*w = (int)sw;
	*h = (int)sh;
	if (!have) {
		snprintf(b, sizeof(b), "carskin: no CARS/%s.nfsv -- the skin is the base colour alone",
			car);
		slog(log, b);
		return skin;
	}

	Scratch s = { (u32*)malloc(n * 4), (u8*)malloc(n), (u8*)malloc(1024 + n), n };
	if (!s.img || !s.mask || !s.p8) {
		snprintf(b, sizeof(b), "carskin: no memory to composite %s -- base colour alone", car);
		slog(log, b);
	} else {
		build_table();
		int vinyls = 0;
		for (int k = 0; k < CARSKIN_VINYLS; k++) {
			const u32 part = d->vinyl[k];
			bool dup = false;
			for (int j = 0; j < k; j++)
				dup |= d->vinyl[j] == part;
			if (!part || dup)
				continue;
			const u8* pe = part_find(&v, part);
			if (!pe || !rd16(pe + 8)) {
				// PORT: a vinyl part with no NUMCOLOURS makes retail's
				// CompositeSkin give up on the whole skin; this leaves out
				// just that layer.
				snprintf(b, sizeof(b), "carskin: %s vinyl 0x%08lx has no NUMCOLOURS -- left off",
					car, (unsigned long)part);
				slog(log, b);
				continue;
			}
			u32 rc[CARSKIN_REMAPS];
			const bool remap = pe[10] != 0;
			for (int j = 0; remap && j < CARSKIN_REMAPS; j++) {
				const u32 c = d->vinylColour[k][j];
				rc[j] = c ? colour(paint, c, 0) : kEmptyRemap[j];
			}
			if (layer(&v, rd32(pe + 4), remap ? rc : NULL, skin, &s))
				vinyls++;
			else {
				snprintf(b, sizeof(b), "carskin: %s vinyl 0x%08lx has no art -- left off",
					car, (unsigned long)part);
				slog(log, b);
			}
		}
		char name[64];
		snprintf(name, sizeof(name), "%s_DOORLINE%s", car, d->doorline);
		const bool door = layer(&v, ext(NO_HASH, name), NULL, skin, &s);

		// The swatches last, each region's colour over its texels.
		int swatched = 0;
		for (int i = 0; i < CARSKIN_REGIONS; i++) {
			const u8* e = v.tab + v.oSwatch + i * NFSV_SWATCH;
			const u32 count = rd32(e);
			for (u32 t = 0; t < count && t < SWATCH_MAX; t++) {
				const u32 at = rd32(e + 4 + t * 4);
				if (at < n)
					skin[at] = region[i];
			}
			swatched += count != 0;
		}
		snprintf(b, sizeof(b), "carskin: %s base 0x%08lx, %d vinyl layer%s, doorline%s%s%s, "
			"%d swatches, in %lu ms", car, (unsigned long)base, vinyls, vinyls == 1 ? "" : "s",
			d->doorline[0] ? " " : " plain", d->doorline, door ? "" : " (missing)", swatched,
			(unsigned long)(osGetTime() - t0));
		slog(log, b);
	}
	free(s.img);
	free(s.mask);
	free(s.p8);
	skin_close(&v);
	return skin;
}

// ------------------------------------------------------------------ rims

// tools/rim_pack.py's layout. Its texture table and data are the .nfsv ones,
// so an Nfsv reads it once the header is in.
#define NFSR_MAGIC    0x5253464Eu  // 'NFSR' little-endian
#define NFSR_VERSION  1
#define NFSR_HDR      0x30
#define NFSR_PART     12

enum { KIND_B8 = 2 };   // byte 0 of each mask texel, all FUN_00612EE0 reads

// size[0] gets DUMMY_WHEEL1's width and height, size[1] DUMMY_SPINNER1's.
static bool nfsr_open(Nfsv* v, const char* path, u32 size[2][2]) {
	memset(v, 0, sizeof(*v));
	v->f = fopen(path, "rb");
	if (!v->f)
		return false;
	u8 h[NFSR_HDR];
	bool ok = fread(h, 1, NFSR_HDR, v->f) == NFSR_HDR && rd32(h) == NFSR_MAGIC &&
	          rd32(h + 4) == NFSR_VERSION;
	if (ok) {
		size[0][0] = rd16(h + 8);  size[0][1] = rd16(h + 10);
		size[1][0] = rd16(h + 12); size[1][1] = rd16(h + 14);
		v->nTex = rd32(h + 16);    v->nPart = rd32(h + 20);
		v->oTex = rd32(h + 24);    v->oPart = rd32(h + 28);
		v->oData = rd32(h + 32);
		ok = (u64)v->oTex + (u64)v->nTex * NFSV_TEX <= v->oData &&
		     (u64)v->oPart + (u64)v->nPart * NFSR_PART <= v->oData &&
		     v->oData <= (1u << 20);
		for (int k = 0; k < 2; k++)
			ok = ok && size[k][0] >= 8 && size[k][1] >= 8 && size[k][0] <= 512 &&
			     size[k][1] <= 512;
	}
	if (ok) {
		v->tab = (u8*)malloc(v->oData);
		ok = v->tab && fseek(v->f, 0, SEEK_SET) == 0 &&
		     fread(v->tab, 1, v->oData, v->f) == v->oData;
	}
	if (!ok)
		nfsv_close(v);
	return ok;
}

// FUN_00612EE0 over `img` in place. A mask byte m = 0 keeps the image's texel.
// Otherwise the texel goes toward FUN_00611A30's modulate of it by `colour`
// (each byte trunc(colour byte / 255 * image byte): s_T) by w1 = m / 255, the
// image keeping w0 = 1 - w1, and FUN_00611B00 sums trunc(byte * w) over the
// weights above 1/255, clamped at 255 -- both products in single precision,
// as s_T's are. Alpha stays the image's.
static void rim_texels(u32* img, const u8* mask, u32 n, u32 colour) {
	const float f = 1.0f / 255.0f;
	for (u32 i = 0; i < n; i++) {
		const u32 m = mask[i];
		if (!m)
			continue;
		const float w1 = (float)m * f, w0 = 1.0f - w1;
		const bool live0 = w0 > f, live1 = w1 > f;
		const u32 px = img[i];
		u32 out = px & 0xFF000000u;
		for (int shift = 0; shift < 24; shift += 8) {
			const u32 c = (px >> shift) & 0xFF;
			u32 acc = 0;
			if (live0)
				acc += (u32)(int)((float)c * w0);
			if (live1)
				acc += (u32)(int)((float)s_T[(colour >> shift) & 0xFF][c] * w1);
			out |= (acc > 255 ? 255 : acc) << shift;
		}
		img[i] = out;
	}
}

int carskin_wheels(const char* dataRoot, const CarSkinDesc* d, const PaintTable* paint,
		CarSkinTex out[CARSKIN_WHEEL_TEXS], void (*log)(const char* msg)) {
	memset(out, 0, sizeof(CarSkinTex) * CARSKIN_WHEEL_TEXS);
	char b[224];
	if (!d->basePaint || !d->frontWheel) {
		snprintf(b, sizeof(b), "carskin: rims not composited -- no %s",
			d->basePaint ? "front wheel part" : "BASE_PAINT (CompositeSkin stops first)");
		slog(log, b);
		return 0;
	}
	char path[320];
	snprintf(path, sizeof(path), "%s/CARS/WHEELS.nfsr", dataRoot);
	Nfsv v;
	u32 size[2][2];
	if (!nfsr_open(&v, path, size)) {
		slog(log, "carskin: no CARS/WHEELS.nfsr -- the rims keep their own art");
		return 0;
	}
	// A front wheel part with neither attribute -- every STOCK RIMS part --
	// gives both composites a missing image, and they stop.
	const u8* pe = find(v.tab + v.oPart, v.nPart, NFSR_PART, d->frontWheel);
	const u32 tn = pe ? rd32(pe + 4) : 0, sp = pe ? rd32(pe + 8) : 0;
	if (!tn && !sp) {
		snprintf(b, sizeof(b), "carskin: front wheel 0x%08lx has no TEXTURE_NAME or "
			"SPINNER_TEXTURE -- nothing to composite", (unsigned long)d->frontWheel);
		slog(log, b);
		nfsv_close(&v);
		return 0;
	}
	const struct {
		u32 img, mask, paint;
		const char* what;
	} job[CARSKIN_WHEEL_TEXS] = {
		{ tn ? ext(tn, "_WHEEL") : 0, ext(tn, "_WHEEL_INNER_MASK"), d->rimPaint, "rim" },
		{ sp, ext(sp, "_MASK"), d->spinnerPaint, "spinner" },
	};
	build_table();
	int filled = 0;
	for (int k = 0; k < CARSKIN_WHEEL_TEXS; k++) {
		if (!job[k].img)
			continue;
		const u64 t0 = osGetTime();
		const u8* te = tex_find(&v, job[k].img);
		const u8* me = tex_find(&v, job[k].mask);
		// FUN_0061DE90 composites only when image, mask and the dummy agree
		// in size; rim_pack.py packs no other pair.
		const u32 w = size[k][0], h = size[k][1], n = w * h;
		if (!te || !me || te[8] != KIND_ARGB || me[8] != KIND_B8 || rd16(te + 4) != w ||
		    rd16(te + 6) != h || rd16(me + 4) != w || rd16(me + 6) != h) {
			snprintf(b, sizeof(b), "carskin: front wheel 0x%08lx's %s 0x%08lx has no art -- "
				"left as it is", (unsigned long)d->frontWheel, job[k].what,
				(unsigned long)job[k].img);
			slog(log, b);
			continue;
		}
		u32* img = (u32*)malloc(n * 4);
		u8* mask = (u8*)malloc(n);
		if (!img || !mask || !inflate_to(&v, te, img, n * 4) || !inflate_to(&v, me, mask, n)) {
			free(img);
			free(mask);
			continue;
		}
		// The colour off the paint part as CompositeSkin's are built, or
		// 0xFFFFFFFF with the slot empty.
		const u32 c = job[k].paint ? colour(paint, job[k].paint, 0xFFFFFFFFu) : 0xFFFFFFFFu;
		rim_texels(img, mask, n, c);
		free(mask);
		out[filled].hash = job[k].img;
		out[filled].argb = img;
		out[filled].w = (int)w;
		out[filled].h = (int)h;
		filled++;
		snprintf(b, sizeof(b), "carskin: %s 0x%08lx of front wheel 0x%08lx, colour 0x%08lx%s, "
			"%lux%lu in %lu ms", job[k].what, (unsigned long)job[k].img,
			(unsigned long)d->frontWheel, (unsigned long)c, job[k].paint ? "" : " (no paint)",
			(unsigned long)w, (unsigned long)h, (unsigned long)(osGetTime() - t0));
		slog(log, b);
	}
	nfsv_close(&v);
	return filled;
}
