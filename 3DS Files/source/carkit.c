// See carkit.h. Both file layouts are documented at the top of
// tools/kit_pack.py, and the constants here mirror them.
#include "carkit.h"
#include "cardamage.h"

_Static_assert(CARKIT_DAMAGE_ITEMS == CARDAMAGE_ITEMS, "carkit.h and cardamage.h disagree");
#include "lightmat.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFSM_MAGIC    0x4D53464Eu  // 'NFSM' little-endian
#define NFSM_VERSION  3           // v1: no decal attributes, so no decals
#define NFSM_HDR      0x20        // v2: no LOD models, so every car is LOD A
#define NFSM_ENTRY_V2 20
#define NFSM_ENTRY    44
#define NFSM_KITNAME  32
#define NFSM_ATTR     8

#define NFSK_MAGIC    0x4B53464Eu  // 'NFSK'
#define NFSK_VERSION  4           // v3: no part pivots, so zero; v2: no light materials, so DEFAULT
#define NFSK_HDR_V1   0x80        // v1: no WheelLine, no material classes
#define NFSK_HDR      0xA0
#define NFSK_TEX      16
#define NFSK_SLOT     8
#define NFSK_PART_V3  32
#define NFSK_PART     44
#define NFSK_BATCH_V2 16
#define NFSK_BATCH    20
#define NFSK_MARKER   72
#define NFSK_VERT     32
#define NFSK_FLAG_WHEELS 1

// Kit batch flags bits 28-29 (kit_pack.py MAT_CLASS_*): which of Render's
// repaintable light materials a sub-mesh is authored with. carkit.c turns a
// match into MODEL3D_BATCH_PAINT_TINT where retail would make the swap, and
// clears the field either way.
#define KIT_MAT_SHIFT 28
#define KIT_MAT_MASK  0x30000000u
#define KIT_MAT_RIM   1           // MAGSILVER, MAGCHROME, MAGGUNMETAL
#define KIT_MAT_BRAKE 2           // CALIPER, CALLIPER

// The light materials Render__13CarRenderInfo (Speed.elf 0x80239018) swaps for
// the car's own with ReplaceLightMaterial before a section draws (TASKS.md
// #166), by the name hash a kit batch carries (kit_pack.py). Every other name
// is looked up as it is, and one the table lacks is DEFAULT.
#define LM_CARSKIN     0xd6d6080au  // "CARSKIN": the section's paint
#define LM_WINDSHIELD  0x471a1dcau  // "WINDSHIELD": the window tint (+0x38c)
#define LM_BODY        0xa7366ae6u  // carbon fibre on a carbon part (+0x3b8), else the body paint
#define LM_MIRROR      0x6bfa56dau  // "MIRROR": the mirror paint, else the body's (0x814(r1))
#define LM_EXHAUST_TIP 0x3b3a1f8du  // "EXHAUST_TIP": the exhaust paint, when it has one (+0x394)
#define LM_AUDIO       0x5be72f1cu  // the audio paint, else the body's (+0x39c)

// What the assembled model is written as: an ordinary v3 .nfsc, exactly the
// layout tools/car_pack.py's pack_nfsc() produces.
#define NFSC_MAGIC    0x4353464Eu
#define NFSC_VERSION  3
#define NFSC_HDR      0x40

#define NO_KIT     0xFF
#define MAX_KITS   14
#define MAX_INST   224        // LOD A's parts, and their B, C and D models
#define MAX_TEX    128
#define MAX_OVER   10
#define NO_HASH    0xFFFFFFFFu

// CarSlotId, the index into a record's PresetCarSlot array (SPEED2.EXE .rdata
// 0x402e1c; docs/formats/savefile.md). Render__13CarRenderInfo numbers the
// same slots one lower -- its GC slot 12 is SLOT_SPOILER here.
enum {
	SLOT_BASE             = 0x01,
	SLOT_FRONT_BUMPER     = 0x02,
	SLOT_REAR_BUMPER      = 0x03,
	SLOT_LEFT_SIDE_MIRROR = 0x04,
	SLOT_RIGHT_SIDE_MIRROR = 0x05,
	SLOT_BODY             = 0x06,
	SLOT_WIDE_BODY        = 0x07,
	SLOT_ROOF             = 0x08,
	SLOT_HOOD             = 0x0a,
	SLOT_SKIRT            = 0x0c,
	SLOT_SPOILER          = 0x0d,
	SLOT_ENGINE           = 0x0e,
	SLOT_HEADLIGHT        = 0x0f,
	SLOT_BRAKELIGHT       = 0x10,
	SLOT_EXHAUST          = 0x11,
	SLOT_DOOR_PANEL_LEFT  = 0x14,
	SLOT_DOOR_SILL_RIGHT  = 0x17,
	SLOT_FENDER           = 0x18,
	SLOT_QUARTER          = 0x19,
	SLOT_FRONT_BRAKE      = 0x1c,
	SLOT_REAR_BRAKE       = 0x1d,
	SLOT_FRONT_WHEEL      = 0x1e,
	SLOT_REAR_WHEEL       = 0x1f,
	SLOT_WING_MIRROR      = 0x21,
	SLOT_LICENSE_PLATE    = 0x22,
	SLOT_TRUNK_AUDIO      = 0x23,
	SLOT_LAST_AUDIO       = 0x2f,
	SLOT_DAMAGE_FRONT     = 0x30,  // .. DAMAGE_TOP 0x34
	SLOT_DECAL_HOOD       = 0x35,  // the decal layouts: HOOD, FRONT_WINDOW,
	SLOT_DECAL_LEFT_DOOR  = 0x38,  // REAR_WINDOW, LEFT/RIGHT_DOOR,
	SLOT_DECAL_RIGHT_QUARTER = 0x3b,  // LEFT/RIGHT_QUARTER
	SLOT_WIDE_DECAL_LEFT_DOOR = 0x3c,  // .. WIDEBODY_DECAL_RIGHT_QUARTER 0x3f
	SLOT_LAST_MODEL       = 0x3f,
	SLOT_BASE_PAINT       = 0x40,
	SLOT_VINYL_LAYER0     = 0x41,  // .. VINYL_LAYER3 0x44
	SLOT_PAINT_ENGINE     = 0x45,
	SLOT_PAINT_SPOILER    = 0x46,
	SLOT_PAINT_BRAKE      = 0x47,
	SLOT_PAINT_EXHAUST    = 0x48,
	SLOT_PAINT_AUDIO      = 0x49,
	SLOT_PAINT_RIM        = 0x4a,
	SLOT_PAINT_SPINNER    = 0x4b,
	SLOT_PAINT_ROOF       = 0x4c,
	SLOT_PAINT_MIRROR     = 0x4d,
	SLOT_VINYL_COLOUR     = 0x4e,  // 4 per vinyl layer, .. 0x5d
	SLOT_DECAL_TEX        = 0x62,  // DECAL_HOOD_TEX0, 8 per layout, .. 0x99
	SLOT_WINDOW_TINT      = 0x9a,
	SLOT_NEON             = 0x9b,
	SLOT_NEON_ENGINE      = 0x9c,
	SLOT_NEON_TRUNK       = 0x9e,
	SLOT_HEADLIGHT_BULB   = 0xa0,
};

// Position markers on the car's _BASE_A part (chunk 0x13401a), by the name
// each hash is the bStringHash of.
#define MARKER_SPOILER      0xc93b73fdu  // "SPOILER"
#define MARKER_ROOF_SCOOP   0x90c81258u  // "ROOF_SCOOP"
#define MARKER_MIRROR_LEFT  0x9b971511u  // "MIRROR_BODY_LEFT"
#define MARKER_MIRROR_RIGHT 0x0ee87ec4u  // "MIRROR_BODY_RIGHT"
// And on a wheel's model 0: how far in from the rim's outer face the brake sits.
#define MARKER_BRAKE        0xea8dfb05u  // "MARKER_BRAKE"

// The six markers CreateCarLightFlares (Speed.elf 0x80235e50) makes flares
// of, with the type it files each under: 0 headlight, 1 brakelight.
static const struct {
	u32 hash;
	u8 type, side;
} kLightMarkers[] = {
	{ 0x9db90133u, CARKIT_LIGHT_HEAD,  CARKIT_LIGHT_LEFT },    // "LEFT_HEADLIGHT"
	{ 0xd09091c6u, CARKIT_LIGHT_HEAD,  CARKIT_LIGHT_RIGHT },   // "RIGHT_HEADLIGHT"
	{ 0x7a5bcf69u, CARKIT_LIGHT_HEAD,  CARKIT_LIGHT_CENTRE },  // "CENTRE_HEADLIGHT"
	{ 0x31a66786u, CARKIT_LIGHT_BRAKE, CARKIT_LIGHT_LEFT },    // "LEFT_BRAKELIGHT"
	{ 0xbf700a79u, CARKIT_LIGHT_BRAKE, CARKIT_LIGHT_RIGHT },   // "RIGHT_BRAKELIGHT"
	{ 0xa2a2fc7cu, CARKIT_LIGHT_BRAKE, CARKIT_LIGHT_CENTRE },  // "CENTRE_BRAKELIGHT"
};

// RenderFlaresOnCar's car types 5, 11, 12, 24, 25 and 30, whose centre brake
// light goes dark under an aftermarket spoiler.
static const char* const kCentreBrakeUnderSpoiler[] = {
	"CIVIC", "TIBURON", "SENTRA", "3000GT", "GTO", "IMPREZAWRX",
};

// Every rim is authored this wide (DAT_008026c4), its outer face at y = 0 and
// the barrel running inboard to +0.184, with the tyre baked in.
#define RIM_AUTHORED_WIDTH  0.184f

// The markers FUN_00638190 files into Render's exhaust list (+0x166) and
// plate list (+0x188). Every car folder carries them on its REAR_BUMPER and
// widebody BODY parts only.
static const u32 kExhaustMarkers[] = {
	0xdf066a41u,  // "CENTRE_EXHAUST"
	0xf21be91eu,  // "EXHAUST_RIGHT"
	0xa5b28001u,  // "EXHAUST"
	0xa2799c2bu,  // "EXHAUST_LEFT"
	0xbcf8a18bu,  // "LEFT_EXHAUST"
	0xbd7cf15eu,  // "RIGHT_EXHAUST"
};
static const u32 kPlateMarkers[] = {
	0x494ed280u,  // "LICENSE_PLATE_REAR"
	0xc805f7e0u,  // "REAR_LICENSE_PLATE"
};
#define MAX_REAR_MARKERS 8

// The slots SwitchSkin (SPEED2.EXE 0x636840) points at the car's composited
// skin, besides <CAR>_SKIN1 itself: the two unnamed body-paint slots
// (tools/car_pack.py SLOT_BODY_OUTER/_TRIM) and the three every library
// part shares -- a spoiler or roof scoop out of the SPOILER or ROOF kit
// samples the car's own skin, at its swatch.
#define TEXSLOT_BODY_OUTER   0x3c84d757u
#define TEXSLOT_BODY_TRIM    0xa7366ae6u
#define TEXSLOT_GLOBAL_SKIN1 0xdda11455u  // "GLOBAL_SKIN1"
#define TEXSLOT_SPOILER_SKIN 0x5c6a5e31u  // "SPOILER_SKIN"
#define TEXSLOT_ROOF_SKIN    0x9f8a1249u  // "ROOF_SKIN"

// The decals (UpdateDecalTextures, SPEED2.EXE 0x61FF40; TASKS.md #296). A
// layout's model carries up to eight positions, sub-meshes keyed by
// DUMMY_DECAL1..8 (consecutive hashes: the names differ only in the last
// character), and each of the seven layouts has a replacement table of its
// own. The four widebody layouts share the door and quarter tables.
#define TEXSLOT_DUMMY_DECAL1 0x910e6654u  // "DUMMY_DECAL1"
#define DECAL_LAYOUTS        7
#define DECAL_POSITIONS      8
#define DECAL_KIT            "DECALS"
// The cracked panes, WINDOW_DAMAGE0..2 (kit_pack.py GLASS_KIT, #324).
#define GLASS_KIT            "GLASS"
// The trunk audio components and their EMPTY placeholders (kit_pack.py
// AUDIO_KIT, #314).
#define AUDIO_KIT            "AUDIO"
static const u32 kGlassTex[3] = { 0x0A155545u, 0x0A155546u, 0x0A155547u };
// The six window slots in UpdateCarReplacementTextures' order, and the rear
// window's REAR_DEFROSTER, which goes with the rear (window 1).
static const u32 kGlassSlot[7] = {
	0x7B220DDFu,   // WINDOW_FRONT
	0x1B0763A0u,   // WINDOW_REAR
	0xE7E4EF49u,   // WINDOW_LEFT_FRONT
	0x4CDEBFCAu,   // WINDOW_LEFT_REAR
	0x60F8B13Cu,   // WINDOW_RIGHT_FRONT
	0x0AB88F5Du,   // WINDOW_RIGHT_REAR
	0xD195BE56u,   // REAR_DEFROSTER
};
static bool s_glass = true;

void carkit_glass(bool on) {
	s_glass = on;
}

static bool s_audio = true;

void carkit_audio(bool on) {
	s_audio = on;
}

static int glass_window(u32 slot) {
	for (int w = 0; w < 7; w++)
		if (kGlassSlot[w] == slot)
			return w == 6 ? 1 : w;
	return -1;
}
// What batch_tex() returns for a position retail leaves at DEFAULTALPHA, a
// 32x32 texture with no coverage anywhere: build_blob() writes it as a
// batch of no indices, which draws exactly that -- nothing.
#define TEX_HIDDEN           (-2)

// CompositeSkin's region colours, in swatch order (carskin.h).
static const u8 kSkinRegions[CARSKIN_REGIONS] = {
	SLOT_PAINT_ENGINE, SLOT_PAINT_SPOILER, SLOT_PAINT_BRAKE, SLOT_PAINT_EXHAUST,
	SLOT_PAINT_AUDIO, SLOT_PAINT_ROOF, SLOT_PAINT_MIRROR,
};
#define SKIN_REGION_AUDIO 4

// The light slots a fitted headlight or brakelight part fills: its texture
// base (part attribute 0x10c98090) + "_OFF" for the lamp and + "_GLASS_OFF"
// for the lens, which is what FUN_00631e20 builds the per-car names from.
// Every light state is an _OFF texture: UpdateLightStateTextures (SPEED2.EXE
// 0x60C940) only picks among them, and a lit lamp is its flares. Speed.elf
// also points the headlight lenses at WINDOW; the PC build does not.
static const u32 kHeadlightSlots[4]  = { 0xa7e6ea53u, 0xa532fc46u, 0x267d188cu, 0xf68ef19fu };
static const u32 kBrakelightSlots[4] = { 0xd947f346u, 0x02b52399u, 0x000bcbbfu, 0x01f40b32u };
static const char* const kLightSuffix[4] = { "_OFF", "_OFF", "_GLASS_OFF", "_GLASS_OFF" };

// Render's per-component materials ([0xe4..0xea]). Each takes its own paint
// only when the part it belongs to is aftermarket and its paint slot is
// filled; otherwise it is the body material [0xe1]. The audio one wants a
// level-3 install, the rest any upgrade at all.
static const struct {
	u8 paint, gate, paintSlot, minLevel;
} kPaintGates[] = {
	{ CARKIT_PAINT_SPOILER, SLOT_SPOILER,     SLOT_PAINT_SPOILER, 1 },
	{ CARKIT_PAINT_ROOF,    SLOT_ROOF,        SLOT_PAINT_ROOF,    1 },
	{ CARKIT_PAINT_MIRROR,  SLOT_WING_MIRROR, SLOT_PAINT_MIRROR,  1 },
	{ CARKIT_PAINT_RIM,     SLOT_FRONT_WHEEL, SLOT_PAINT_RIM,     1 },
	{ CARKIT_PAINT_EXHAUST, SLOT_EXHAUST,     SLOT_PAINT_EXHAUST, 1 },
	{ CARKIT_PAINT_BRAKE,   SLOT_FRONT_BRAKE, SLOT_PAINT_BRAKE,   1 },
	{ CARKIT_PAINT_AUDIO,   SLOT_TRUNK_AUDIO, SLOT_PAINT_AUDIO,   3 },
};

_Static_assert(CARKIT_PAINT_SLOTS == MODEL3D_CAR_PAINT_SLOTS,
	"carkit.h and model3d.h disagree on the paint slots");

static u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static float rdf(const u8* p) { float v; memcpy(&v, p, 4); return v; }

// bStringHash carried on from `h` (seed 0xFFFFFFFF for a plain hash). The
// retail hash upper-cases first; every name built here already is.
static u32 ext(u32 h, const char* s) {
	while (*s)
		h = h * 33u + (u8)*s++;
	return h;
}

// ------------------------------------------------------------------ tables

typedef struct {
	u8* buf;
	u32 n, nKits, nAttrs;
	u32 entSize;      // NFSM_ENTRY, or NFSM_ENTRY_V2 for a table with no LODs
	const u8* ents;
	const u8* kits;
	const u8* attrs;
} PartModels;

static bool pm_load(const char* path, PartModels* pm) {
	memset(pm, 0, sizeof(*pm));
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* b = (size >= NFSM_HDR) ? (u8*)malloc((size_t)size) : NULL;
	bool ok = b && fread(b, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	if (ok && (rd32(b) != NFSM_MAGIC || rd32(b + 4) < 1 || rd32(b + 4) > NFSM_VERSION))
		ok = false;
	if (ok) {
		pm->n = rd32(b + 8);
		pm->nKits = rd32(b + 12);
		pm->entSize = rd32(b + 4) >= 3 ? NFSM_ENTRY : NFSM_ENTRY_V2;
		const u32 eo = rd32(b + 16), ko = rd32(b + 20);
		ok = (u64)eo + (u64)pm->n * pm->entSize <= (u64)size &&
		     (u64)ko + (u64)pm->nKits * NFSM_KITNAME <= (u64)size;
		pm->ents = b + eo;
		pm->kits = b + ko;
		if (ok && rd32(b + 4) >= 2) {
			pm->nAttrs = rd32(b + 0x18);
			const u32 ao = rd32(b + 0x1c);
			ok = (u64)ao + (u64)pm->nAttrs * NFSM_ATTR <= (u64)size;
			pm->attrs = b + ao;
		}
	}
	if (!ok) {
		free(b);
		memset(pm, 0, sizeof(*pm));
		return false;
	}
	pm->buf = b;
	return true;
}

static const u8* pm_find(const PartModels* pm, u32 hash) {
	u32 lo = 0, hi = pm->n;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		const u8* e = pm->ents + mid * pm->entSize;
		const u32 h = rd32(e);
		if (h == hash)
			return e;
		if (h < hash) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

// The one attribute a part carries in the table (kit_pack.py
// build_decal_attrs): a layout's SHAPE, a decal's NAME, a paint's
// LIGHT_MATERIAL or a wheel's INNER_RADIUS.
static bool pm_attr(const PartModels* pm, u32 hash, u32* value) {
	u32 lo = 0, hi = pm->nAttrs;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		const u8* e = pm->attrs + mid * NFSM_ATTR;
		const u32 h = rd32(e);
		if (h == hash) {
			*value = rd32(e + 4);
			return true;
		}
		if (h < hash) lo = mid + 1; else hi = mid;
	}
	return false;
}

// Entry fields.
static u32 pe_model(const u8* e, int mi)  { return rd32(e + 4 + mi * 4); }
static u32 pe_light(const u8* e)          { return rd32(e + 12); }
static u8  pe_kit(const u8* e, int mi)    { return e[16 + mi]; }
static u8  pe_level(const u8* e)          { return e[18]; }
// Model `mi`'s LOD `l` (1 = B .. 3 = D) model, 0 for none or a v2 table.
static u32 pe_lod(const PartModels* pm, const u8* e, int mi, int l) {
	return pm->entSize >= NFSM_ENTRY ? rd32(e + 20 + (mi * 3 + l - 1) * 4) : 0;
}

// --------------------------------------------------------------------- kits

typedef struct {
	char name[NFSM_KITNAME];
	FILE* f;
	bool tried;
	u32 version;
	u32 batchSize;       // NFSK_BATCH, or NFSK_BATCH_V2 before v3
	u32 partSize;        // NFSK_PART, or NFSK_PART_V3 before v4
	u32 nTex, nParts, nBatch, nVert, nIdx, nMarker, nSlot, texOff, texSize, flags;
	float wheel[4][5];   // { x, y, z, radius, width } per corner
	float line[4][2];    // { fender, wide } per corner, signed like y (v2 only)
	u8* tab;             // file bytes [header, vertOff)
	u32 oSlot, oPart, oBatch, oMarker;
	u32 vertOff, idxOff;
	bool cached;         // a copy of s_kc's: its file and table are the cache's
} Kit;

static void kit_close(Kit* k) {
	if (k->f)
		fclose(k->f);
	free(k->tab);
	k->f = NULL;
	k->tab = NULL;
}

// ------------------------------------------------------------------- cache
// #402. The tables and kits an assembly opened, and every run it read out of
// a kit, kept between builds while held (carkit_cache_hold). A body shop
// preview changes one part, so the next build asks for nearly the same runs.

#define KC_BUDGET (4u << 20)   // bytes of kit runs kept, past which reads go to the card

typedef struct {
	char kit[NFSM_KITNAME];
	u32 off, size;
	u8* data;
} KcRun;

static struct {
	bool hold;              // shop || load
	bool shop;              // carkit_cache_hold(): everything, runs too
	bool load;              // carkit_cache_hold_load(): PARTMODELS and the kits only
	char root[320];
	PartModels pm;          // pm.buf NULL until a build loads it
	Kit kits[MAX_KITS];     // opened (or tried and missing) kits, f NULL for missing
	int nKits;
	KcRun* run;
	int nRun, capRun;
	u32 bytes;
	u32 hits, misses;       // reads served from RAM / off the card, per build
} s_kc;

static void kc_free(void) {
	for (int i = 0; i < s_kc.nKits; i++)
		kit_close(&s_kc.kits[i]);
	for (int i = 0; i < s_kc.nRun; i++)
		free(s_kc.run[i].data);
	free(s_kc.run);
	free(s_kc.pm.buf);
	const bool shop = s_kc.shop, load = s_kc.load;
	memset(&s_kc, 0, sizeof(s_kc));
	s_kc.shop = shop;
	s_kc.load = load;
	s_kc.hold = shop || load;
}

static void kc_set(bool shop, bool load) {
	s_kc.shop = shop;
	s_kc.load = load;
	s_kc.hold = shop || load;
	if (!s_kc.hold && (s_kc.nKits || s_kc.nRun || s_kc.pm.buf))
		kc_free();
}

void carkit_cache_hold(bool hold) {
	kc_set(hold, s_kc.load);
}

void carkit_cache_hold_load(bool hold) {
	kc_set(s_kc.shop, hold);
}

bool carkit_cache_held(void) {
	return s_kc.shop;
}

// Drops whatever was kept for another data root (never seen: the root is the
// card's one folder).
static void kc_root(const char* root) {
	if (strcmp(s_kc.root, root) != 0) {
		kc_free();
		snprintf(s_kc.root, sizeof(s_kc.root), "%s", root);
	}
}

// `name`'s kit out of the cache into `k`, which then shares its file and
// table. False when it was never opened while held.
static bool kc_take(Kit* k, const char* name) {
	if (!s_kc.hold)
		return false;
	for (int i = 0; i < s_kc.nKits; i++)
		if (strcmp(s_kc.kits[i].name, name) == 0) {
			*k = s_kc.kits[i];
			k->cached = true;
			return true;
		}
	return false;
}

// A kit an assembly opened, handed to the cache instead of closed.
static bool kc_put(Kit* k) {
	if (!s_kc.hold || k->cached || s_kc.nKits >= MAX_KITS)
		return false;
	s_kc.kits[s_kc.nKits++] = *k;
	k->f = NULL;
	k->tab = NULL;
	return true;
}

// The run at `off`/`size` of `k` out of the cache into `dst`, if an earlier
// build read it.
static bool kc_lookup(const Kit* k, u32 off, void* dst, u32 size) {
	if (!s_kc.hold)
		return false;
	for (int i = 0; i < s_kc.nRun; i++) {
		const KcRun* r = &s_kc.run[i];
		if (r->off == off && r->size == size && strcmp(r->kit, k->name) == 0) {
			memcpy(dst, r->data, size);
			s_kc.hits++;
			return true;
		}
	}
	return false;
}

// A run just read off the card, kept while the cache is held.
static void kc_store(const Kit* k, u32 off, const void* src, u32 size) {
	if (!s_kc.shop || s_kc.bytes + size > KC_BUDGET)
		return;
	if (s_kc.nRun == s_kc.capRun) {
		const int cap = s_kc.capRun ? s_kc.capRun * 2 : 256;
		KcRun* n = (KcRun*)realloc(s_kc.run, (size_t)cap * sizeof(KcRun));
		if (!n)
			return;
		s_kc.run = n;
		s_kc.capRun = cap;
	}
	u8* c = (u8*)malloc(size);
	if (!c)
		return;
	memcpy(c, src, size);
	KcRun* r = &s_kc.run[s_kc.nRun++];
	memcpy(r->kit, k->name, sizeof(r->kit));
	r->off = off;
	r->size = size;
	r->data = c;
	s_kc.bytes += size;
}

// `size` bytes at `off` in `k`'s file, from RAM when an earlier build read
// the same run.
static bool kit_read(const Kit* k, u32 off, void* dst, u32 size) {
	if (!size || kc_lookup(k, off, dst, size))
		return true;
	if (!k->f || fseek(k->f, (long)off, SEEK_SET) != 0 || fread(dst, 1, size, k->f) != size)
		return false;
	s_kc.misses++;
	kc_store(k, off, dst, size);
	return true;
}

// Many kit runs read as a batch: the ones the cache holds come from RAM, the
// rest are sorted by file and offset and neighbours (gap <= KR_GAP, span <=
// KR_SPAN) share one seek+read. A car's blob asks for ~200 small runs (each
// part instance's vertices and indices, then every texture), mostly close
// together in the kit, and on the SD card each separate seek+read costs far
// more than the bytes between them.
typedef struct {
	const Kit* k;
	u32 off, size;
	u8* dst;
} KRead;

// The last build_blob()'s batch: ms reading and KB asked for (#348).
static u32 s_blobReadMs, s_blobReadKB;

#define KR_GAP  (16u * 1024u)
#define KR_SPAN (512u * 1024u)

static int kread_cmp(const void* a, const void* b) {
	const KRead* x = (const KRead*)a;
	const KRead* y = (const KRead*)b;
	if (x->k != y->k)
		return x->k < y->k ? -1 : 1;
	return x->off < y->off ? -1 : x->off > y->off ? 1 : 0;
}

static bool kit_read_batch(KRead* rs, int n) {
	int m = 0; // misses compacted to the front
	for (int i = 0; i < n; i++) {
		if (!rs[i].size || kc_lookup(rs[i].k, rs[i].off, rs[i].dst, rs[i].size))
			continue;
		rs[m++] = rs[i];
	}
	if (!m)
		return true;
	qsort(rs, (size_t)m, sizeof(KRead), kread_cmp);
	u8* tmp = NULL;
	u32 tmpCap = 0;
	bool ok = true;
	for (int i = 0; i < m && ok;) {
		const Kit* k = rs[i].k;
		const u32 lo = rs[i].off;
		u32 hi = rs[i].off + rs[i].size;
		int j = i + 1;
		while (j < m && rs[j].k == k && rs[j].off <= hi + KR_GAP) {
			const u32 e = rs[j].off + rs[j].size;
			const u32 nhi = e > hi ? e : hi;
			if (nhi - lo > KR_SPAN)
				break;
			hi = nhi;
			j++;
		}
		if (j == i + 1) {
			// A lone run reads straight into its destination.
			ok = k->f && fseek(k->f, (long)lo, SEEK_SET) == 0
				&& fread(rs[i].dst, 1, rs[i].size, k->f) == rs[i].size;
			if (ok) {
				s_kc.misses++;
				kc_store(k, lo, rs[i].dst, rs[i].size);
			}
			i = j;
			continue;
		}
		const u32 span = hi - lo;
		if (span > tmpCap) {
			free(tmp);
			tmp = (u8*)malloc(span);
			tmpCap = tmp ? span : 0;
		}
		if (!tmp) {
			// No room for the span: fall back to one read per run.
			for (; i < j && ok; i++)
				ok = kit_read(rs[i].k, rs[i].off, rs[i].dst, rs[i].size);
			continue;
		}
		ok = k->f && fseek(k->f, (long)lo, SEEK_SET) == 0 && fread(tmp, 1, span, k->f) == span;
		if (ok) {
			s_kc.misses++;
			for (const int i0 = i; i < j; i++) {
				memcpy(rs[i].dst, tmp + (rs[i].off - lo), rs[i].size);
				// A part placed twice (both mirrors, all four wheels) asks
				// for the same run more than once: keep it once.
				if (i == i0 || rs[i - 1].off != rs[i].off || rs[i - 1].size != rs[i].size)
					kc_store(k, rs[i].off, rs[i].dst, rs[i].size);
			}
		}
	}
	free(tmp);
	return ok;
}

static bool kit_open(Kit* k, const char* root, void (*log)(const char*)) {
	k->tried = true;
	char path[320];
	snprintf(path, sizeof(path), "%s/CARS/%s.nfsk", root, k->name);
	k->f = fopen(path, "rb");
	char b[200];
	if (!k->f) {
		if (log) {
			snprintf(b, sizeof(b), "carkit: no CARS/%s.nfsk", k->name);
			log(b);
		}
		return false;
	}
	u8 h[NFSK_HDR];
	fseek(k->f, 0, SEEK_END);
	const long size = ftell(k->f);
	fseek(k->f, 0, SEEK_SET);
	bool ok = size >= NFSK_HDR_V1 && fread(h, 1, NFSK_HDR_V1, k->f) == NFSK_HDR_V1 &&
	          rd32(h) == NFSK_MAGIC;
	k->version = ok ? rd32(h + 4) : 0;
	ok = ok && k->version >= 1 && k->version <= NFSK_VERSION;
	k->batchSize = k->version >= 3 ? NFSK_BATCH : NFSK_BATCH_V2;
	k->partSize = k->version >= 4 ? NFSK_PART : NFSK_PART_V3;
	// A v1 kit (packed before #204 slice 3) still assembles; it just has no
	// wheel lines, so its car keeps the old wheel stand-in (place_wheels_v1).
	const u32 hdr = k->version >= 2 ? NFSK_HDR : NFSK_HDR_V1;
	if (ok && hdr > NFSK_HDR_V1)
		ok = size >= (long)hdr &&
		     fread(h + NFSK_HDR_V1, 1, hdr - NFSK_HDR_V1, k->f) == hdr - NFSK_HDR_V1;
	if (ok) {
		k->nTex = rd32(h + 0x08);   k->nParts = rd32(h + 0x0c);
		k->nBatch = rd32(h + 0x10); k->nVert = rd32(h + 0x14);
		k->nIdx = rd32(h + 0x18);   k->nMarker = rd32(h + 0x1c);
		k->nSlot = rd32(h + 0x20);  k->texOff = rd32(h + 0x24);
		k->texSize = rd32(h + 0x28); k->flags = rd32(h + 0x2c);
		for (int c = 0; c < 4; c++)
			for (int i = 0; i < 5; i++)
				k->wheel[c][i] = rdf(h + 0x30 + c * 20 + i * 4);
		for (int c = 0; c < 4 && hdr > NFSK_HDR_V1; c++)
			for (int i = 0; i < 2; i++)
				k->line[c][i] = rdf(h + 0x80 + c * 8 + i * 4);
		k->oSlot = k->nTex * NFSK_TEX;
		k->oPart = k->oSlot + k->nSlot * NFSK_SLOT;
		k->oBatch = k->oPart + k->nParts * k->partSize;
		k->oMarker = k->oBatch + k->nBatch * k->batchSize;
		const u32 tabSize = k->oMarker + k->nMarker * NFSK_MARKER;
		k->vertOff = hdr + tabSize;
		k->idxOff = k->vertOff + k->nVert * NFSK_VERT;
		ok = (u64)k->idxOff + (u64)k->nIdx * 2 <= k->texOff &&
		     (u64)k->texOff + k->texSize <= (u64)size;
		if (ok) {
			k->tab = (u8*)malloc(tabSize ? tabSize : 1);
			ok = k->tab && fread(k->tab, 1, tabSize, k->f) == tabSize;
		}
	}
	if (!ok) {
		if (log) {
			snprintf(b, sizeof(b), "carkit: CARS/%s.nfsk is not a v1-v%d kit", k->name,
				NFSK_VERSION);
			log(b);
		}
		kit_close(k);
		return false;
	}
	return true;
}

static const u8* kit_part(const Kit* k, u32 hash) {
	u32 lo = 0, hi = k->nParts;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		const u8* p = k->tab + k->oPart + mid * k->partSize;
		const u32 h = rd32(p);
		if (h == hash)
			return p;
		if (h < hash) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

static int kit_tex(const Kit* k, u32 hash) {
	u32 lo = 0, hi = k->nTex;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		const u32 h = rd32(k->tab + mid * NFSK_TEX);
		if (h == hash)
			return (int)mid;
		if (h < hash) lo = mid + 1; else hi = mid;
	}
	return -1;
}

// The texture a slot resolves to by default, 0 for flat paint.
static u32 kit_slot(const Kit* k, u32 slot) {
	u32 lo = 0, hi = k->nSlot;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		const u8* s = k->tab + k->oSlot + mid * NFSK_SLOT;
		const u32 h = rd32(s);
		if (h == slot)
			return rd32(s + 4);
		if (h < slot) lo = mid + 1; else hi = mid;
	}
	return 0;
}

static bool kit_marker(const Kit* k, u32 partHash, u32 markerHash, float m[16]) {
	for (u32 i = 0; i < k->nMarker; i++) {
		const u8* r = k->tab + k->oMarker + i * NFSK_MARKER;
		if (rd32(r) == partHash && rd32(r + 4) == markerHash) {
			memcpy(m, r + 8, 64);
			return true;
		}
	}
	return false;
}

// A texture's run in the blob. Entries and their data are both written in
// hash order (kit_pack.py), so a run ends where the next one starts.
static u32 kit_tex_size(const Kit* k, int i) {
	const u32 off = rd32(k->tab + i * NFSK_TEX + 12);
	const u32 end = (u32)(i + 1) < k->nTex ? rd32(k->tab + (i + 1) * NFSK_TEX + 12)
	                                       : k->texSize;
	return end > off ? end - off : 0;
}

// ---------------------------------------------------------------- assembly

typedef struct {
	Kit* kit;
	const u8* part;
	float m[16];      // row vectors, p' = p * m (the retail bMatrix4 convention)
	bool mirrorY;     // flip y before m: the left-hand brakes
	u8 paint;
	u8 slot;
	u8 tint;          // KIT_MAT_* whose sub-meshes take `paint` over their texture
	u8 lods;          // MODEL3D_BATCH_LOD_* bits it is drawn at, 0 = every level
	// MODEL3D_BATCH_CORNER_* / _WHEEL_* bits, from bit 20: the wheel part and
	// corner it is (place_wheels), or 0.
	u8 wheel;
	// MODEL3D_BATCH_PART* bits (part_tag()): the opening part it moves or
	// shows with, or 0.
	u32 partFl;
} Inst;

typedef struct {
	const char* root;
	const char* car;
	const PartModels* pm;
	const unsigned int* slots;
	int numSlots;
	bool enabled[SLOT_LAST_MODEL + 1];  // enable_slots()
	Kit kits[MAX_KITS];
	int nKits;
	Kit* carKit;
	Inst inst[MAX_INST];
	int nInst;
	u32 skinTex;      // <CAR>_SKIN1 in the car's kit, 0 when it has none
	u32 overSlot[MAX_OVER], overTex[MAX_OVER];
	int nOver;
	u32 decalTex[DECAL_LAYOUTS][DECAL_POSITIONS];  // 0 = DEFAULTALPHA
	int nDecals;
	Kit* decalKit;
	bool staleLods;   // a kit without the LOD models the table names, logged once
	// The light materials (TASKS.md #166): each paint slot's, [0] the body's,
	// and the window tint's, as choose_light_mats() found them; then one
	// lightmat_index() per batch build_blob() wrote, for model3d. batchMat is
	// NULL without GLOBAL/LIGHTMATS.nfsl.
	const LightMat* paintMat[CARKIT_PAINT_SLOTS];
	const LightMat* tintMat;
	u8* batchMat;
	u32 nBatchMat;
	// The glass (#324): each window batch build_blob() wrote, which window it
	// is, and the blob's three cracked panes. nGlass 0 for a car without them.
	int* glassBatch;
	u8* glassWin;
	int nGlass;
	s32 glassTex[3];
	float rim[4][16]; // place_wheels(): each corner's rim placement, FL FR RR RL
	int rims;
	float pivot[4][3];   // and where each corner's parts turn (model3d_set_car_pivots)
	u32 partTag;      // the partFl push_inst() gives new instances (part_tag())
	bool bayLit;      // place_bay() added MODEL3D_BATCH_PART_LIT instances
	void (*log)(const char*);
} Asm;

static void alog(const Asm* a, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void alog(const Asm* a, const char* fmt, ...) {
	if (!a->log)
		return;
	char b[224];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	a->log(b);
}

static Kit* get_kit_named(Asm* a, const char* name) {
	for (int i = 0; i < a->nKits; i++)
		if (strcmp(a->kits[i].name, name) == 0)
			return a->kits[i].f ? &a->kits[i] : NULL;
	if (a->nKits >= MAX_KITS)
		return NULL;
	Kit* k = &a->kits[a->nKits++];
	memset(k, 0, sizeof(*k));
	snprintf(k->name, sizeof(k->name), "%s", name);
	if (kc_take(k, k->name))
		return k->f ? k : NULL;
	return kit_open(k, a->root, a->log) ? k : NULL;
}

static Kit* get_kit(Asm* a, u8 index) {
	if (index == NO_KIT || index >= a->pm->nKits)
		return NULL;
	char name[NFSM_KITNAME + 1];
	memcpy(name, a->pm->kits + index * NFSM_KITNAME, NFSM_KITNAME);
	name[NFSM_KITNAME] = '\0';
	return get_kit_named(a, name);
}

static const u8* slot_entry(const Asm* a, int s) {
	if (s < 0 || s >= a->numSlots || !a->slots[s])
		return NULL;
	return pm_find(a->pm, a->slots[s]);
}

static int slot_level(const Asm* a, int s) {
	const u8* e = slot_entry(a, s);
	return e ? pe_level(e) : 0;
}

// The model a slot's part draws as `mi` (0 or 1), and the kit it lives in.
static u32 slot_model(Asm* a, int s, int mi, Kit** kit) {
	*kit = NULL;
	const u8* e = slot_entry(a, s);
	if (!e)
		return 0;
	const u32 mh = pe_model(e, mi);
	if (!mh)
		return 0;
	if (pe_kit(e, mi) == NO_KIT) {
		// In no geometry file at all -- TIBURON's KITW00_BODY and FULLROOF,
		// say. Retail skips a model it cannot find, and so does this.
		return 0;
	}
	*kit = get_kit(a, pe_kit(e, mi));
	return *kit ? mh : 0;
}

static void ident(float m[16]) {
	memset(m, 0, 64);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static Inst* push_inst(Asm* a, Kit* k, const u8* p, const float m[16], bool mirrorY,
		u8 paint, int slot, u8 tint, u8 lods) {
	if (a->nInst >= MAX_INST)
		return NULL;
	Inst* in = &a->inst[a->nInst++];
	in->kit = k;
	in->part = p;
	memcpy(in->m, m, 64);
	in->mirrorY = mirrorY;
	in->paint = paint;
	in->slot = (u8)slot;
	in->tint = tint;
	in->lods = lods;
	in->wheel = 0;
	in->partFl = a->partTag;
	return in;
}

// The car LODs (CarRenderInfo::Render, Speed.elf 0x80239018). Render picks one
// level for the car (world_car_lod) and draws each part's model out of its
// row of LOD models (+0x179c, part * 32) at that level, skipping the part
// when the row has none -- the A model's row too, so a part with no A model
// is never drawn. The wheels are the exception: their section (0x8023cd00)
// takes the level clamped to C, and the brakes are drawn in the same section.
static bool wheel_section(int slot) {
	return slot >= SLOT_FRONT_BRAKE && slot <= SLOT_REAR_WHEEL;
}

// Adds the part's B, C and D models beside the A one `in` already is, each
// tagged with the car levels it is drawn at, and tags `in` as A's. A model
// no PARTMODELS row names that way -- a stock wheel or brake standing in for
// one not on the card -- is drawn at every level, as is every part of a kit
// packed without its LOD models.
static void add_lods(Asm* a, Inst* in, u32 hash) {
	const u8* e = slot_entry(a, in->slot);
	int mi = -1;
	for (int i = 0; e && i < 2; i++)
		if (pe_model(e, i) == hash && pe_kit(e, i) != NO_KIT)
			mi = i;
	if (mi < 0 || a->pm->entSize < NFSM_ENTRY)
		return;
	const u8* lp[MODEL3D_CAR_LODS] = { in->part, NULL, NULL, NULL };
	for (int l = 1; l < MODEL3D_CAR_LODS; l++) {
		const u32 h = pe_lod(a->pm, e, mi, l);
		if (!h)
			continue;
		lp[l] = kit_part(in->kit, h);
		if (!lp[l]) {
			if (!a->staleLods)
				alog(a, "carkit: %s.nfsk has no LOD models -- repack it with tools/kit_pack.py; "
					"its parts are drawn at LOD A at every distance", in->kit->name);
			a->staleLods = true;
			return;
		}
	}
	// lods[v]: the car levels part level v is drawn at.
	u8 lods[MODEL3D_CAR_LODS] = { 0, 0, 0, 0 };
	for (int L = 0; L < MODEL3D_CAR_LODS; L++) {
		const int v = (wheel_section(in->slot) && L > 2) ? 2 : L;
		if (lp[v])
			lods[v] |= (u8)(1u << L);
	}
	in->lods = lods[0];
	const Inst A = *in;
	for (int v = 1; v < MODEL3D_CAR_LODS; v++)
		if (lods[v])
			push_inst(a, A.kit, lp[v], A.m, A.mirrorY, A.paint, A.slot, A.tint, lods[v]);
}

static bool add_inst(Asm* a, Kit* k, u32 hash, const float m[16], bool mirrorY,
		u8 paint, int slot, u8 tint) {
	const u8* p = k ? kit_part(k, hash) : NULL;
	if (!p) {
		alog(a, "carkit: slot 0x%02x model 0x%08lx is not in %s.nfsk", slot,
			(unsigned long)hash, k ? k->name : "?");
		return false;
	}
	Inst* in = push_inst(a, k, p, m, mirrorY, paint, slot, tint, 0);
	if (!in)
		return false;
	add_lods(a, in, hash);
	return true;
}

// LOD A's instances: the ones every level or bit 0 takes.
static bool inst_is_a(const Inst* in) {
	return !in->lods || (in->lods & 1u);
}

// A car whose levels together pass u16 indexing keeps LOD A alone, drawn at
// every distance, rather than not being built at all.
static void fit_lods(Asm* a) {
	u32 nVert = 0;
	for (int i = 0; i < a->nInst; i++)
		nVert += rd32(a->inst[i].part + 16);
	if (nVert <= 65535)
		return;
	int n = 0;
	for (int i = 0; i < a->nInst; i++) {
		if (!inst_is_a(&a->inst[i]))
			continue;
		a->inst[n] = a->inst[i];
		a->inst[n++].lods = 0;
	}
	alog(a, "carkit: %s with its LOD models is %lu vertices -- LOD A only",
		a->car, (unsigned long)nVert);
	a->nInst = n;
}

static void add_override(Asm* a, u32 slot, u32 tex) {
	if (a->nOver < MAX_OVER && kit_tex(a->carKit, tex) >= 0) {
		a->overSlot[a->nOver] = slot;
		a->overTex[a->nOver] = tex;
		a->nOver++;
	}
}

// What Render__13CarRenderInfo draws, and where. Decoded from the PC build
// (FUN_006219xx) and Speed.elf; TASKS.md #204 carries the evidence.
//
//   * The main loop draws models 0 and 1 of every loaded slot in car space,
//     except the ones with placement code of their own: the side mirrors, the
//     roof scoop, the spoiler, the engine and the exhaust (GC 3, 4, 7, 12, 13,
//     16) and GC 27..46 -- brakes, wheels, wing mirror, plate and audio.
//   * The door panels and sills are drawn only with their door open or in
//     the frontend, and the hood / trunk undersides only with them open:
//     part_tag() marks them conditional and model3d.c decides each draw.
//   * The decal layouts (0x35..0x3f) are drawn like any other part, their
//     positions textured by choose_decals(). The damage slots before them
//     are not drawn yet.
//   * The spoiler and the roof scoop hang on the base's SPOILER and
//     ROOF_SCOOP markers, or sit in car space on a base without one.
//   * The wing mirror hangs on MIRROR_BODY_LEFT/RIGHT, but only when the
//     mirror part is aftermarket (the stock one is authored in place). The
//     right marker carries a negated Y row and Render negates m[5] back, so
//     both come out unrotated.
//   * Rims, spinners and brakes go on the four CarTypeInfo corners, seated on
//     the fender line (place_wheels()).
//   * The exhaust tips and the plate hang on markers of the drawn parts
//     (place_rear()), so they follow the rear bumper the save fitted.
//   * Of those, only the slots RideInfo left enabled (enable_slots()).
//
// The engine and the audio install have blocks of their own after the main
// loop (place_bay()).
static bool main_loop_draws(int s) {
	if (s == SLOT_LEFT_SIDE_MIRROR || s == SLOT_RIGHT_SIDE_MIRROR ||
	    s == SLOT_ROOF || s == SLOT_SPOILER || s == SLOT_ENGINE || s == SLOT_EXHAUST)
		return false;
	if (s >= SLOT_FRONT_BRAKE && s <= SLOT_LAST_AUDIO)
		return false;
	return s < SLOT_DAMAGE_FRONT || s >= SLOT_DECAL_HOOD;
}

// RideInfo's part-enable pass (FUN_0061BCD0, run after every slot install;
// TASKS.md #206). It starts every slot enabled, and Render's main loop and
// FUN_00638190's marker walk both skip a disabled one (the byte array at
// RideInfo+0x838, read at 0x6230e2). What it switches:
//
//   * An aftermarket widebody (WIDE_BODY level > 0) is the whole shell: it
//     turns off both bumpers, the skirt, the stock body, the fenders and the
//     quarters, and the plain door decals for the widebody's own. Without one
//     it is the other way round, WIDE_BODY itself off -- a car never draws
//     both, nor collects both sets of exhaust and plate markers.
//   * Attribute 0xE80A3B62 on a part names slots to turn off. Every shipped
//     carrier (the TRAP and GATO hoods) names DECAL_HOOD, which the exe's
//     table maps to GC 0x25 (TRUNK_AUDIO_COMP_2), not drawn here; the packer
//     leaves it out.
//   * FRONT_WHEEL goes off on a runtime flag ([0x839BC1]) that stays clear
//     for a car on show, so it is left on.
static void enable_slots(Asm* a) {
	for (int s = 0; s <= SLOT_LAST_MODEL; s++)
		a->enabled[s] = true;
	const bool wide = slot_level(a, SLOT_WIDE_BODY) > 0;
	static const u8 plain[] = { SLOT_FRONT_BUMPER, SLOT_REAR_BUMPER, SLOT_SKIRT,
	                            SLOT_BODY, SLOT_FENDER, SLOT_QUARTER };
	for (size_t i = 0; i < sizeof(plain); i++)
		a->enabled[plain[i]] = !wide;
	a->enabled[SLOT_WIDE_BODY] = wide;
	for (int i = 0; i < 4; i++) {
		a->enabled[SLOT_DECAL_LEFT_DOOR + i] = !wide;
		a->enabled[SLOT_WIDE_DECAL_LEFT_DOOR + i] = wide;
	}
}

static bool draws(const Asm* a, int s) {
	return main_loop_draws(s) && a->enabled[s];
}

// ShowTrunkUnderInFE (Speed.elf 0x802313ec): the car types whose trunk
// underside the frontend shows with the trunk shut, by name.
static bool trunk_under_in_fe(const char* car) {
	static const char* const kCars[] = {
		"PEUGOT", "FOCUS", "COROLLA", "240SX", "PEUGOT106", "CORSA", "HUMMER",
		"NAVIGATOR", "ESCALADE", "TIBURON", "CELICA", "SUPRA", "GOLF", "A3", "RSX",
		"ECLIPSE", "TT", "350Z", "3000GT", "RX7",
	};
	for (size_t i = 0; i < sizeof(kCars) / sizeof(kCars[0]); i++)
		if (strcmp(car, kCars[i]) == 0)
			return true;
	return false;
}

// The opening part slot s's model mi goes with, as MODEL3D_BATCH_PART* bits
// (model3d.h), or 0. GetCarSlotAnim's table (GLOBALB.BUN chunk 0x34608, one
// location per GC slot -- one lower than s -- and model; -1 none) for the
// slots carkit draws, and Render__13CarRenderInfo's two overrides: the rear
// window decal goes with the trunk on a ShowTrunkUnderInFE car, and the
// HUMMER's headlights with its hood. The door panels and sills (which the
// table leaves out; a sill never moves) and the hood / trunk undersides are
// the conditional ones Render shows only as model3d.h says.
static u32 part_tag(const Asm* a, int s, int mi) {
	int loc = -1;
	u32 fl = 0;
	switch (s - 1) {
	case 0x09: case 0x34:             loc = 0; break;  // HOOD, DECAL_HOOD
	case 0x19: loc = 0; fl = MODEL3D_BATCH_PART_COND; break;  // HOOD_UNDER
	case 0x0a:                        loc = 1; break;  // TRUNK
	case 0x1a:                                         // TRUNK_UNDER
		loc = 1;
		fl = MODEL3D_BATCH_PART_COND |
		     (trunk_under_in_fe(a->car) ? MODEL3D_BATCH_PART_SUB : 0);
		break;
	case 0x0c: if (mi == 0) loc = 1; break;            // SPOILER
	case 0x0f: if (mi == 1) loc = 1; break;            // BRAKELIGHT
	case 0x0e: if (strcmp(a->car, "HUMMER") == 0) loc = 0; break;  // HEADLIGHT
	case 0x36: if (trunk_under_in_fe(a->car)) loc = 1; break;  // DECAL_REAR_WINDOW
	case 0x11: case 0x37: case 0x3b: loc = 2; break;  // DOOR_LEFT, the door decals
	case 0x12: case 0x38: case 0x3c: loc = 3; break;
	case 0x13: loc = 2; fl = MODEL3D_BATCH_PART_COND; break;  // DOOR_PANEL_LEFT
	case 0x14: loc = 3; fl = MODEL3D_BATCH_PART_COND; break;
	case 0x15: loc = 2; fl = MODEL3D_BATCH_PART_COND | MODEL3D_BATCH_PART_STILL; break;
	case 0x16: loc = 3; fl = MODEL3D_BATCH_PART_COND | MODEL3D_BATCH_PART_STILL; break;
	default: break;
	}
	if (loc < 0)
		return 0;
	if (loc == 0 && mi == 1)
		fl |= MODEL3D_BATCH_PART_SUB;  // a split hood's second half
	return MODEL3D_BATCH_PART | fl | ((u32)loc << MODEL3D_BATCH_CORNER_SHIFT);
}

static void place_on_marker(Asm* a, int s, u32 marker, u8 paint, const u8* base) {
	float m[16];
	ident(m);
	if (base && kit_marker(a->carKit, rd32(base), marker, m))
		m[3] = m[7] = m[11] = 0.0f, m[15] = 1.0f;
	for (int mi = 0; mi < 2; mi++) {
		Kit* k;
		const u32 mh = slot_model(a, s, mi, &k);
		a->partTag = part_tag(a, s, mi);
		if (mh)
			add_inst(a, k, mh, m, false, paint, s, 0);
		a->partTag = 0;
	}
}

static bool hash_in(u32 h, const u32* set, size_t n) {
	for (size_t i = 0; i < n; i++)
		if (set[i] == h)
			return true;
	return false;
}

// m = R * m, R being FUN_005bada0(m, m, 0x4000): a quarter turn about Y,
// rows (c,0,-s) (0,1,0) (s,0,c) with c = 0, s = 1. The exhaust and plate
// markers are authored pitched a quarter turn (x axis up, z axis out of the
// back of the car), and this turns them back to car orientation: the tips
// come out unrotated and the plate keeps only the tilt its bumper gives it.
static void quarter_turn_y(float m[16]) {
	float r0[4], r2[4];
	for (int j = 0; j < 4; j++) {
		r0[j] = -m[8 + j];
		r2[j] = m[j];
	}
	memcpy(m, r0, sizeof(r0));
	memcpy(m + 8, r2, sizeof(r2));
}

// The exhaust tips and the plate. FUN_00638190 walks the position markers of
// model 0 of every enabled slot and files each exhaust marker into Render's
// list +0x166 and each plate marker into +0x188. Render then draws the EXHAUST
// slot's models 0 and 1 once per exhaust marker, with the exhaust material,
// and the LICENSE_PLATE slot's once per plate marker, each at R * marker.
// Only rear bumpers and widebody bodies carry these markers, so walking the
// slots the main loop draws finds the same set -- and only one of the two is
// ever enabled.
//
// On ten car types (the hatches) Render puts the plate on the trunk lid's
// matrix rather than the car's, so it swings up with an opened trunk. This
// port never opens the trunk, and closed the two are the same.
static void place_rear(Asm* a) {
	float mk[2][MAX_REAR_MARKERS][16];
	int n[2] = { 0, 0 };
	for (int s = SLOT_BASE; s <= SLOT_LAST_MODEL; s++) {
		if (!draws(a, s))
			continue;
		Kit* k;
		const u32 mh = slot_model(a, s, 0, &k);
		if (!mh)
			continue;
		for (u32 i = 0; i < k->nMarker; i++) {
			const u8* r = k->tab + k->oMarker + i * NFSK_MARKER;
			if (rd32(r) != mh)
				continue;
			const u32 h = rd32(r + 4);
			const int w = hash_in(h, kExhaustMarkers, sizeof(kExhaustMarkers) / 4) ? 0
			            : hash_in(h, kPlateMarkers, sizeof(kPlateMarkers) / 4) ? 1 : -1;
			if (w < 0 || n[w] >= MAX_REAR_MARKERS)
				continue;
			float* m = mk[w][n[w]++];
			memcpy(m, r + 8, 64);
			m[3] = m[7] = m[11] = 0.0f, m[15] = 1.0f;
			quarter_turn_y(m);
		}
	}
	static const struct {
		u8 slot, paint;
	} rear[2] = {
		{ SLOT_EXHAUST,       CARKIT_PAINT_EXHAUST },
		{ SLOT_LICENSE_PLATE, CARKIT_PAINT_BODY },
	};
	for (int w = 0; w < 2; w++) {
		for (int mi = 0; mi < 2; mi++) {
			Kit* k;
			const u32 mh = slot_model(a, rear[w].slot, mi, &k);
			for (int i = 0; mh && i < n[w]; i++)
				add_inst(a, k, mh, mk[w][i], false, rear[w].paint, rear[w].slot, 0);
		}
	}
}

// The underglow's tubes (TASKS.md #257). UpdateCarParts (Speed.elf 0x80233fcc)
// takes the models of three slots -- FRONT_BUMPER, SKIRT and REAR_BUMPER, or
// the WIDE_BODY for all three when an aftermarket widebody is fitted, since
// that one part carries every neon marker -- and asks each for
// FRONT_NEON_<i>_START/_END, SIDE_NEON_<i>_... and REAR_NEON_<i>_... in turn,
// i = 0..2. A pair goes in only when both markers are there, and a marker's
// translation is the tube end in car space. RenderNeon's first call draws
// these; the engine and trunk tubes are collected below, the cabin's never
// lights (see carneon.h).
static void collect_neon(Asm* a, CarKitInfo* info) {
	static const char* const prefix[3] = { "FRONT_NEON_", "SIDE_NEON_", "REAR_NEON_" };
	const bool wide = slot_level(a, SLOT_WIDE_BODY) > 0;
	const u8 slot[3] = {
		wide ? SLOT_WIDE_BODY : SLOT_FRONT_BUMPER,
		wide ? SLOT_WIDE_BODY : SLOT_SKIRT,
		wide ? SLOT_WIDE_BODY : SLOT_REAR_BUMPER,
	};
	info->neonPart = SLOT_NEON < a->numSlots ? a->slots[SLOT_NEON] : 0;
	for (int j = 0; j < 3; j++) {
		Kit* k;
		const u32 mh = slot_model(a, slot[j], 0, &k);
		for (int i = 0; mh && i < 3 && info->neonSegs < CARKIT_NEON_SEGMENTS; i++) {
			char name[24];
			snprintf(name, sizeof(name), "%s%d_", prefix[j], i);
			const u32 h = ext(NO_HASH, name);
			float ms[16], me[16];
			if (!kit_marker(k, mh, ext(h, "START"), ms) || !kit_marker(k, mh, ext(h, "END"), me))
				continue;
			CarKitNeonSeg* sg = &info->neon[info->neonSegs++];
			memcpy(sg->a, ms + 12, sizeof(sg->a));
			memcpy(sg->b, me + 12, sizeof(sg->b));
		}
	}
	// The engine bay's and the trunk's (TASKS.md #506): the CarRenderInfo
	// constructor's ENGINE_NEON_START_%d / _END_%d (n < 5) on the ENGINE
	// slot's model 0 and MARKER_TRUNK_NEON_START_%d / _END_%d (n < 4) on the
	// TRUNK_AUDIO layout's model 0 (the strings are SPEED2.EXE's). Both
	// models are authored in car space, as place_bay() draws them.
	static const struct {
		u8 slot, max;
		const char* start;
		const char* end;
	} bay[2] = {
		{ SLOT_ENGINE,      CARKIT_ENGINE_NEON, "ENGINE_NEON_START_",       "ENGINE_NEON_END_" },
		{ SLOT_TRUNK_AUDIO, CARKIT_TRUNK_NEON,  "MARKER_TRUNK_NEON_START_", "MARKER_TRUNK_NEON_END_" },
	};
	for (int b = 0; b < 2; b++) {
		CarKitNeonSeg* out = b ? info->trunkNeon : info->engineNeon;
		int* segs = b ? &info->trunkNeonSegs : &info->engineNeonSegs;
		Kit* k;
		const u32 mh = slot_model(a, bay[b].slot, 0, &k);
		for (int i = 0; mh && i < bay[b].max; i++) {
			char ns[32], ne[32];
			snprintf(ns, sizeof(ns), "%s%d", bay[b].start, i);
			snprintf(ne, sizeof(ne), "%s%d", bay[b].end, i);
			float ms[16], me[16];
			if (!kit_marker(k, mh, ext(NO_HASH, ns), ms) || !kit_marker(k, mh, ext(NO_HASH, ne), me))
				continue;
			CarKitNeonSeg* sg = &out[(*segs)++];
			memcpy(sg->a, ms + 12, sizeof(sg->a));
			memcpy(sg->b, me + 12, sizeof(sg->b));
		}
	}
	// The save's NEON_ENGINE and NEON_TRUNK colours (CarSlotId 0x9C / 0x9E,
	// Render's GC parts 0x9b / 0x9d), and Render's level-3 tests on the
	// ENGINE and TRUNK_AUDIO parts (GC 0xd / 0x22) for the bay light.
	info->engineNeonPart = SLOT_NEON_ENGINE < a->numSlots ? a->slots[SLOT_NEON_ENGINE] : 0;
	info->trunkNeonPart = SLOT_NEON_TRUNK < a->numSlots ? a->slots[SLOT_NEON_TRUNK] : 0;
	info->bayLit[0] = slot_level(a, SLOT_ENGINE) == 3;
	info->bayLit[1] = slot_level(a, SLOT_TRUNK_AUDIO) == 3;
}

// The light flares (TASKS.md #296). CreateCarLightFlares (Speed.elf
// 0x80235e50, SPEED2.EXE 0x6204d0) walks model 0 of every model slot, enabled
// or not, and makes a flare of each of the six light markers it finds there:
// at the marker's translation, facing along its third row, less the BODY
// model's pivot: CarRenderInfo+0x30, which its constructor (0x802314c0) sets
// from eModel::GetPivotPosition of slot BODY's model 0 just before. A v3 kit
// has no pivots and reads as zero, which every shipped BODY_A but
// COROLLA_KITW02's (1 cm each way) is.
static void collect_lights(Asm* a, CarKitInfo* info) {
	info->bulbPart = SLOT_HEADLIGHT_BULB < a->numSlots ? a->slots[SLOT_HEADLIGHT_BULB] : 0;
	float pivot[3] = { 0.0f, 0.0f, 0.0f };
	{
		Kit* bk;
		const u32 bh = slot_model(a, SLOT_BODY, 0, &bk);
		const u8* bp = bh && bk->version >= 4 ? kit_part(bk, bh) : NULL;
		for (int c = 0; bp && c < 3; c++)
			pivot[c] = rdf(bp + 32 + c * 4);
	}
	if (slot_level(a, SLOT_SPOILER) > 0) {
		for (size_t i = 0; i < sizeof(kCentreBrakeUnderSpoiler) / sizeof(kCentreBrakeUnderSpoiler[0]); i++)
			if (strcmp(a->car, kCentreBrakeUnderSpoiler[i]) == 0)
				info->centreBrakeOff = true;
	}
	for (int s = SLOT_BASE; s <= SLOT_LAST_MODEL; s++) {
		Kit* k;
		const u32 mh = slot_model(a, s, 0, &k);
		for (u32 i = 0; mh && i < k->nMarker; i++) {
			const u8* r = k->tab + k->oMarker + i * NFSK_MARKER;
			if (rd32(r) != mh)
				continue;
			const u32 h = rd32(r + 4);
			for (size_t j = 0; j < sizeof(kLightMarkers) / sizeof(kLightMarkers[0]); j++) {
				if (kLightMarkers[j].hash != h)
					continue;
				if (info->lights >= CARKIT_LIGHTS) {
					alog(a, "carkit: %s has more than %d light markers", a->car, CARKIT_LIGHTS);
					return;
				}
				CarKitLight* l = &info->light[info->lights++];
				for (int c = 0; c < 3; c++) {
					l->pos[c] = rdf(r + 8 + (12 + c) * 4) - pivot[c];
					l->dir[c] = rdf(r + 8 + (8 + c) * 4);
				}
				l->type = kLightMarkers[j].type;
				l->side = kLightMarkers[j].side;
			}
		}
	}
}

// UpdateCarParts' hood nozzle positions (Speed.elf 0x802334D0): the two
// markers on the BASE slot's model 0, render slot 0 there.
static void collect_purge(Asm* a, CarKitInfo* info) {
	static const u32 kHood[2] = { 0xE3DE69DBu, 0x601A6CCEu };
	Kit* k;
	const u32 mh = slot_model(a, SLOT_BASE, 0, &k);
	for (int i = 0; i < 2; i++) {
		float m[16];
		info->purgeHoodOk[i] = mh && kit_marker(k, mh, kHood[i], m);
		if (info->purgeHoodOk[i])
			memcpy(info->purgeHood[i], m + 12, sizeof(info->purgeHood[i]));
	}
}

// CarRenderInfo::CarRenderInfo's InitSectorPositions (Speed.elf 0x8024C4E4)
// for #324: each breakable item's marker, wherever on the car's drawn models
// it is, as the marker's own translation (x, y), the last found winning.
static void collect_damage(Asm* a, CarKitInfo* info) {
	info->damageItems = 0;
	for (int i = 0; i < a->nInst; i++) {
		const Inst* in = &a->inst[i];
		if (!inst_is_a(in))
			continue;
		const u32 ph = rd32(in->part);
		for (int k = 0; k < CARKIT_DAMAGE_ITEMS; k++) {
			float m[16];
			if (!kit_marker(in->kit, ph, cardamage_item_marker[k], m))
				continue;
			info->damageItem[k][0] = m[12];
			info->damageItem[k][1] = m[13];
			info->damageItems |= 1u << k;
		}
	}
}

// CarRenderInfo's exhaust list (#317): the constructor (Speed.elf 0x802314C0)
// walks the position markers of model 0 of every enabled part and files each
// of the six exhaust markers into list 10 (CarRenderInfo+0x58C), at the
// marker's translation with no flips -- the same markers place_rear() hangs
// the tips on. TriggerEffect puts the nitrous flame (CARFX_NITRO) there.
static void collect_exhaust(Asm* a, CarKitInfo* info) {
	info->exhausts = 0;
	for (int s = SLOT_BASE; s <= SLOT_LAST_MODEL; s++) {
		if (!draws(a, s))
			continue;
		Kit* k;
		const u32 mh = slot_model(a, s, 0, &k);
		if (!mh)
			continue;
		for (u32 i = 0; i < k->nMarker && info->exhausts < CARKIT_EXHAUSTS; i++) {
			const u8* r = k->tab + k->oMarker + i * NFSK_MARKER;
			if (rd32(r) != mh || !hash_in(rd32(r + 4), kExhaustMarkers, sizeof(kExhaustMarkers) / 4))
				continue;
			float m[16];
			memcpy(m, r + 8, 64);
			memcpy(info->exhaust[info->exhausts++], m + 12, sizeof(info->exhaust[0]));
		}
	}
}

// CarRenderInfo's glass position lists (#513): the constructor (Speed.elf
// 0x802314C0, its marker switch at 0x80232DC0..) files these markers of model
// 0 of every enabled part, at the marker's translation with no flips, into
// lists 12..17 (the windows) and 18..23 (both headlights, both brakelights,
// then each one). The XECD_*_Glass emitters burst at 12..17 and 20..23.
static const struct {
	u32 marker;
	u8  list;   // CARKIT_GLASS_LISTS' index: posType 12..17 -> 0..5, 20..23 -> 6..9
} kGlassMarkers[] = {
	{ 0x6EC7E79Fu, 0 }, { 0x7B220DDFu, 0 },   // 12 windshield
	{ 0x3F68F920u, 1 }, { 0x1B0763A0u, 1 },   // 13 rear window
	{ 0x3DF28F69u, 2 }, { 0xE7E4EF49u, 2 },   // 14 left front
	{ 0xE898CFAAu, 3 }, { 0x4CDEBFCAu, 3 },   // 15 left rear
	{ 0x64B0BB1Cu, 4 }, { 0x60F8B13Cu, 4 },   // 16 right front
	{ 0x2F96D0FDu, 5 }, { 0x0AB88F5Du, 5 },   // 17 right rear
	{ 0x9DB90133u, 6 }, { 0xA7E6EA53u, 6 },   // 20 headlight left
	{ 0xD09091C6u, 7 }, { 0xA532FC46u, 7 },   // 21 headlight right
	{ 0x31A66786u, 8 }, { 0xD947F346u, 8 },   // 22 brakelight left
	{ 0xBF700A79u, 9 }, { 0x02B52399u, 9 },   // 23 brakelight right
};

static void collect_glass(Asm* a, CarKitInfo* info) {
	memset(info->glassPosN, 0, sizeof(info->glassPosN));
	for (int s = SLOT_BASE; s <= SLOT_LAST_MODEL; s++) {
		if (!draws(a, s))
			continue;
		Kit* k;
		const u32 mh = slot_model(a, s, 0, &k);
		if (!mh)
			continue;
		for (u32 i = 0; i < k->nMarker; i++) {
			const u8* r = k->tab + k->oMarker + i * NFSK_MARKER;
			if (rd32(r) != mh)
				continue;
			const u32 h = rd32(r + 4);
			for (size_t g = 0; g < sizeof(kGlassMarkers) / sizeof(kGlassMarkers[0]); g++) {
				if (kGlassMarkers[g].marker != h)
					continue;
				const int l = kGlassMarkers[g].list;
				if (info->glassPosN[l] < CARKIT_GLASS_POS) {
					float m[16];
					memcpy(m, r + 8, 64);
					memcpy(info->glassPos[l][info->glassPosN[l]++], m + 12,
						sizeof(info->glassPos[0][0]));
				}
				break;
			}
		}
	}
}

// The car's own part named <car><suffix>: what a wheel or brake slot falls
// back to when the part the save fitted lives in a kit that is not on the card.
static u32 stock_model(const Asm* a, const char* suffix) {
	return ext(ext(NO_HASH, a->car), suffix);
}

static bool slot_filled(const Asm* a, int s) {
	return s < a->numSlots && a->slots[s] != 0;
}

// What RenderTireMask (Speed.elf 0x80236918, SPEED2.EXE 0x620600) reads: the
// matrices the rims are drawn with, and the front wheel part's
// KEY_INNER_RADIUS (GetPart(0x1d)), its rim size in inches. Retail takes 17
// when the RideInfo has no front wheel part.
// PORT: and so does a PARTMODELS.nfsm packed before the rim sizes were carried.
static void collect_rims(const Asm* a, CarKitInfo* info) {
	memcpy(info->rim, a->rim, sizeof(info->rim));
	info->rims = a->rims;
	u32 v = 0;
	const bool have = slot_filled(a, SLOT_FRONT_WHEEL) &&
		pm_attr(a->pm, a->slots[SLOT_FRONT_WHEEL], &v);
	info->rimInches = have ? (float)(s8)v : CARKIT_RIM_INCHES;
}

// Rims, spinners and brakes, put on the four CarTypeInfo corners (FL, FR,
// RR, RL) the way Render__13CarRenderInfo puts them there. Decoded from the PC
// build: Render (FUN_00620ae0), the per-corner offsets it reads (FUN_0061fe10)
// and the marker pass (FUN_00638190); TASKS.md #204 slice 3 has the evidence.
//
//   * A rim, the wheel part's model 0, is scaled across its width to the
//     corner's tyre width when that axle's wheel is aftermarket (s = w / 0.184)
//     and drawn as authored when it is stock. Its outer face lands on the
//     fender line, or on the widebody line under an aftermarket WIDE_BODY, and
//     the barrel runs inboard from there.
//   * The mesh is the right-hand corner's (#126). The left corners get it
//     turned half a turn about z -- a rotation, not a mirror, so a directional
//     spoke pattern keeps its handedness.
//   * The spinner, model 1, sits exactly where the rim does, pushed outward
//     by the front wheel part's SPINNEROFFSET attribute. Only the 31 STOCK
//     RIMS parts carry that attribute and none of them has a model 1, so the
//     push is 0 for every spinner the game ships and is not carried.
//   * The brakes sit p inboard of the same line and are mirrored, not turned,
//     for the left corners: p is the wheel model's MARKER_BRAKE depth times s,
//     or half the tyre width for a wheel without the marker. Render takes both
//     axles' brakes from the FRONT_BRAKE part: model 0 at the front, and at the
//     rear model 1 (its REAR_BRAKE_A caliper) when it is stock, else model 0.
//   * Model 0 of each wheel takes the rim paint on its MAGSILVER, MAGCHROME
//     and MAGGUNMETAL sub-meshes only when the front wheel is aftermarket and
//     the save filled the rim paint slot; otherwise it keeps its own material.
//     The PC build never applies the spinner paint ([0xeb] is always 0). The
//     brakes take the brake paint on CALIPER/CALLIPER under the same kind of
//     gate.
//
// That is the car parked. Every part placed here carries its corner and kind
// (model3d.h MODEL3D_BATCH_CORNER_*), and model3d turns it about the corner's
// pivot as the wheel moves (model3d_set_car_wheels): the record's x and z,
// and y where Render's frame has the wheel matrix -- half the tyre width
// inboard of the line under an aftermarket rim, on it under a stock one
// (SPEED2.EXE: the width times 0.5, 0x8026c0, is 0 at stock). The front
// corners' steering z offset is left out: 0.03 times the camber tweak
// (0x7a6758 x 0x7a6750), which nothing sets.
//
// A part whose kit is not on the card falls back to the car's own stock wheel
// or brake, placed as if the slot were stock and with no paint of its own --
// it is not the part the save's paint was chosen for.
static void tag_wheel(Asm* a, int first, int corner, int kind) {
	for (int i = first; i < a->nInst; i++)
		a->inst[i].wheel = (u8)(kind << 2 | corner);
}

static void place_wheels(Asm* a) {
	const Kit* ck = a->carKit;
	const bool wide = slot_level(a, SLOT_WIDE_BODY) > 0;
	const bool rimPaint = slot_level(a, SLOT_FRONT_WHEEL) > 0 && slot_filled(a, SLOT_PAINT_RIM);
	const int brakeLevel = slot_level(a, SLOT_FRONT_BRAKE);
	const bool brakePaint = brakeLevel > 0 && slot_filled(a, SLOT_PAINT_BRAKE);
	Kit* fk;
	const bool spinners = slot_model(a, SLOT_FRONT_WHEEL, 1, &fk) != 0;

	for (int axle = 0; axle < 2; axle++) {
		const int ws = axle ? SLOT_REAR_WHEEL : SLOT_FRONT_WHEEL;
		Kit* wk;
		u32 wm = slot_model(a, ws, 0, &wk);
		int level = slot_level(a, ws);
		const bool ownRim = wm != 0;
		if (!ownRim) {
			wk = a->carKit;
			wm = stock_model(a, "_KIT00_FRONT_WHEEL_A");
			level = 0;
		}
		Kit* sk = NULL;
		const u32 sm = (ownRim && spinners) ? slot_model(a, ws, 1, &sk) : 0;

		Kit* bk;
		u32 bm = slot_model(a, SLOT_FRONT_BRAKE, (axle && !brakeLevel) ? 1 : 0, &bk);
		const bool ownBrake = bm != 0;
		if (!ownBrake) {
			bk = a->carKit;
			bm = stock_model(a, axle ? "_KIT00_REAR_BRAKE_A" : "_KIT00_FRONT_BRAKE_A");
		}

		float mk[16];
		const float markerY = kit_marker(wk, wm, MARKER_BRAKE, mk) ? mk[13] : 0.0f;
		bool rimB = false;   // a rear rim with a B model to swap to
		for (int c = axle * 2; c < axle * 2 + 2; c++) {
			const float* w = ck->wheel[c];
			const float width = level ? w[4] : RIM_AUTHORED_WIDTH;
			const float s = width / RIM_AUTHORED_WIDTH;
			const float line = ck->line[c][wide ? 1 : 0];
			const float side = w[1] > 0.0f ? -1.0f : 1.0f;   // -1: a left corner
			float m[16];
			ident(m);
			m[0] = side;
			m[5] = side * s;
			m[12] = w[0];
			m[13] = line;
			m[14] = w[2];
			memcpy(a->rim[c], m, sizeof(m));
			a->rims = 4;
			a->pivot[c][0] = w[0];
			a->pivot[c][1] = line + side * 0.5f * (level ? width : 0.0f);
			a->pivot[c][2] = w[2];
			int first = a->nInst;
			add_inst(a, wk, wm, m, false, ownRim ? CARKIT_PAINT_RIM : CARKIT_PAINT_BODY, ws,
				(ownRim && rimPaint) ? KIT_MAT_RIM : 0);
			// The rim and its LOD siblings. A rear one's tag is also what the
			// in-game TireFace swap to the B model tests: Render tests only
			// model 0 of the rear wheel, not the front rims, the spinners or
			// the brakes.
			tag_wheel(a, first, c, MODEL3D_WHEEL_RIM);
			for (int i = first; axle && i < a->nInst; i++)
				rimB |= (a->inst[i].lods & 2u) != 0;
			if (sm) {
				first = a->nInst;
				add_inst(a, sk, sm, m, false, CARKIT_PAINT_RIM, ws, 0);
				tag_wheel(a, first, c, MODEL3D_WHEEL_SPINNER);
			}

			const float p = markerY != 0.0f ? s * markerY : 0.5f * width;
			ident(m);
			m[12] = w[0];
			m[13] = line + side * p;
			m[14] = w[2];
			first = a->nInst;
			add_inst(a, bk, bm, m, side < 0.0f,
				ownBrake ? CARKIT_PAINT_BRAKE : CARKIT_PAINT_BODY, SLOT_FRONT_BRAKE,
				(ownBrake && brakePaint) ? KIT_MAT_BRAKE : 0);
			tag_wheel(a, first, c, MODEL3D_WHEEL_BRAKE);
		}
		alog(a, "carkit: %s rims 0x%08lx from %s%s (x%.2f wide, %s line), brakes 0x%08lx from %s%s%s",
			axle ? "rear" : "front", (unsigned long)wm, wk->name, sm ? " + spinners" : "",
			(level ? ck->wheel[axle * 2][4] : RIM_AUTHORED_WIDTH) / RIM_AUTHORED_WIDTH,
			wide ? "wide" : "fender", (unsigned long)bm, bk->name,
			(ownBrake && brakePaint) ? " painted" : "", rimB ? "; LOD B when turned away" : "");
	}
}

// A v1 car kit has no wheel lines to seat a rim on, so it keeps what this file
// drew before slice 3: the car's own stock wheel and brakes, centred on each
// corner's centre plane and mirrored for the left corners, as car_pack.py's
// stock bake still has them.
static void place_wheels_v1(Asm* a) {
	static const struct {
		u8 slot, c0, c1;
		const char* stock;
	} corners[] = {
		{ SLOT_FRONT_WHEEL, 0, 3, "_KIT00_FRONT_WHEEL_A" },
		{ SLOT_FRONT_BRAKE, 0, 1, "_KIT00_FRONT_BRAKE_A" },
		{ SLOT_REAR_BRAKE,  2, 3, "_KIT00_REAR_BRAKE_A" },
	};
	for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
		const u32 mh = stock_model(a, corners[i].stock);
		const u8* p = kit_part(a->carKit, mh);
		if (!p)
			continue;
		const float yc = rdf(p + 28);
		for (int c = corners[i].c0; c <= corners[i].c1; c++) {
			const float* w = a->carKit->wheel[c];
			const bool mirror = w[1] > 0.0f;
			float m[16];
			ident(m);
			m[12] = w[0];
			m[13] = mirror ? w[1] + yc : w[1] - yc;
			m[14] = w[2];
			add_inst(a, a->carKit, mh, m, mirror, CARKIT_PAINT_BODY, corners[i].slot, 0);
		}
	}
}

// The engine bay and the trunk audio install (TASKS.md #314): Render's two
// blocks after the main loop (Speed.elf 0x8023df44 .. 0x8023e660,
// tools/re/wip314/trunkaudio_spec.md). Each at LOD A whatever the car's
// level, in car space, tagged for model3d.c (MODEL3D_BATCH_PART_LIT /
// _AUDIO):
//   * the ENGINE slot's models 0 and 1, only with the hood not closed;
//     model 0 under the light scaled by the hood's swing x0.6;
//   * on the frontend car or the player's (s_audio) the TRUNK_AUDIO layout's
//     models 0 and 1, open trunk or not, model 0 under the trunk's x0.9;
//   * and component i on the layout's MARKER_AUDIO_COMP_i (none without the
//     marker), under the trunk's light, in the component paint: in the
//     frontend the installed part's model (its _PAINT_A model 1 above trunk
//     level 1), in the world the AUDIO_COMP_EMPTY_<size> placeholder of the
//     same paint choice, size by sizeTable (0x8033c11c).
// With a level-3 ENGINE / TRUNK_AUDIO part, Render adds that bay's neon
// colour ([0xd9] / [0xdf]) to the scaled light: CarKitInfo bayLit, applied
// by model3d.c from Model3DPartPose neonAdd (#506).
static void push_bay(Asm* a, Kit* k, u32 hash, const float m[16], u8 paint, int slot,
		u32 partFl) {
	const u8* p = k ? kit_part(k, hash) : NULL;
	if (!p)
		return;
	a->partTag = partFl;
	if (push_inst(a, k, p, m, false, paint, slot, 0, 0) && (partFl & MODEL3D_BATCH_PART_LIT))
		a->bayLit = true;
	a->partTag = 0;
}

static void place_bay(Asm* a) {
	float id[16];
	ident(id);
	const u32 still = MODEL3D_BATCH_PART | MODEL3D_BATCH_PART_STILL;
	const u32 trunk = still | (1u << MODEL3D_BATCH_CORNER_SHIFT);
	for (int mi = 0; mi < 2; mi++) {
		Kit* k;
		const u32 mh = slot_model(a, SLOT_ENGINE, mi, &k);
		if (mh)
			push_bay(a, k, mh, id, CARKIT_PAINT_BODY, SLOT_ENGINE,
				still | MODEL3D_BATCH_PART_COND | (mi ? 0 : MODEL3D_BATCH_PART_LIT));
	}
	if (!s_audio)
		return;
	Kit* bk = NULL;
	const u32 box = slot_model(a, SLOT_TRUNK_AUDIO, 0, &bk);
	for (int mi = 0; mi < 2; mi++) {
		Kit* k;
		const u32 mh = slot_model(a, SLOT_TRUNK_AUDIO, mi, &k);
		if (mh)
			push_bay(a, k, mh, id, CARKIT_PAINT_BODY, SLOT_TRUNK_AUDIO,
				trunk | (mi ? 0 : MODEL3D_BATCH_PART_LIT));
	}
	if (!box || !bk)
		return;
	static const u8 kSize[12] = { 0, 0, 1, 1, 2, 2, 0, 0, 0, 0, 1, 1 };
	static const char* const kSizeName[3] = { "SMALL", "MEDIUM", "LARGE" };
	const int paintSel = slot_level(a, SLOT_TRUNK_AUDIO) > 1;
	Kit* ak = get_kit_named(a, AUDIO_KIT);
	int n = 0;
	for (int i = 0; i < 12; i++) {
		char name[40];
		snprintf(name, sizeof(name), "MARKER_AUDIO_COMP_%d", i);
		float m[16];
		if (!kit_marker(bk, box, ext(NO_HASH, name), m))
			continue;
		m[3] = m[7] = m[11] = 0.0f, m[15] = 1.0f;
		const int s = SLOT_TRUNK_AUDIO + 1 + i;
		const u32 comp = trunk | MODEL3D_BATCH_PART_LIT | MODEL3D_BATCH_PART_AUDIO;
		Kit* k;
		const u32 mh = slot_model(a, s, paintSel, &k);
		if (mh)
			push_bay(a, k, mh, m, CARKIT_PAINT_AUDIO, s, comp | MODEL3D_BATCH_PART_SUB);
		snprintf(name, sizeof(name), "AUDIO_COMP_EMPTY_%s%s_A", kSizeName[kSize[i]],
			paintSel ? "_PAINT" : "");
		if (ak)
			push_bay(a, ak, ext(NO_HASH, name), m, CARKIT_PAINT_AUDIO, s, comp);
		n++;
	}
	if (n)
		alog(a, "carkit: trunk audio level %d, %d component markers%s", slot_level(a,
			SLOT_TRUNK_AUDIO), n, ak ? "" : " -- no CARS/" AUDIO_KIT ".nfsk, no placeholders");
}

static void assemble_parts(Asm* a) {
	float id[16];
	ident(id);
	for (int s = SLOT_BASE; s <= SLOT_LAST_MODEL; s++) {
		if (!draws(a, s))
			continue;
		for (int mi = 0; mi < 2; mi++) {
			Kit* k;
			const u32 mh = slot_model(a, s, mi, &k);
			a->partTag = part_tag(a, s, mi);
			if (mh && add_inst(a, k, mh, id, false, CARKIT_PAINT_BODY, s, 0) && a->partTag)
				alog(a, "carkit: slot 0x%02x model %d opens with location %lu%s%s", s, mi,
					(unsigned long)((a->partTag & MODEL3D_BATCH_CORNER_MASK) >>
					                MODEL3D_BATCH_CORNER_SHIFT),
					(a->partTag & MODEL3D_BATCH_PART_COND) ? " (conditional)" : "",
					(a->partTag & MODEL3D_BATCH_PART_STILL) ? " (still)" : "");
			a->partTag = 0;
		}
	}

	Kit* bk;
	const u32 baseHash = slot_model(a, SLOT_BASE, 0, &bk);
	const u8* base = (baseHash && bk == a->carKit) ? kit_part(bk, baseHash) : NULL;
	place_on_marker(a, SLOT_SPOILER, MARKER_SPOILER, CARKIT_PAINT_SPOILER, base);
	place_on_marker(a, SLOT_ROOF, MARKER_ROOF_SCOOP, CARKIT_PAINT_ROOF, base);

	const bool mirrorOnMarker = slot_level(a, SLOT_WING_MIRROR) > 0;
	for (int mi = 0; mi < 2; mi++) {
		Kit* k;
		const u32 mh = slot_model(a, SLOT_WING_MIRROR, mi, &k);
		if (!mh)
			continue;
		float m[16];
		ident(m);
		if (mirrorOnMarker && base &&
		    kit_marker(a->carKit, rd32(base), mi ? MARKER_MIRROR_RIGHT : MARKER_MIRROR_LEFT, m)) {
			m[3] = m[7] = m[11] = 0.0f, m[15] = 1.0f;
			if (mi)
				m[5] = -m[5];
		}
		add_inst(a, k, mh, m, false, CARKIT_PAINT_MIRROR, SLOT_WING_MIRROR, 0);
	}
	place_rear(a);
	place_bay(a);

	if (!(a->carKit->flags & NFSK_FLAG_WHEELS)) {
		alog(a, "carkit: %s.nfsk has no wheel table -- no wheels or brakes", a->car);
		return;
	}
	if (a->carKit->version >= 2)
		place_wheels(a);
	else
		place_wheels_v1(a);
}

// The doorline set the skin takes and the light states this car's parts
// call for.
static void choose_textures(Asm* a, const char** doorline) {
	// FUN_00612b70/FUN_00612c40: a widebody wins, then any aftermarket front
	// bumper, rear bumper or skirt, else the plain set.
	const char* v = "";
	if (slot_level(a, SLOT_WIDE_BODY) > 0)
		v = "_WIDEBODY";
	else if (slot_level(a, SLOT_FRONT_BUMPER) > 0 || slot_level(a, SLOT_REAR_BUMPER) > 0 ||
	         slot_level(a, SLOT_SKIRT) > 0)
		v = "_KIT";
	*doorline = v;

	for (int which = 0; which < 2; which++) {
		const u8* e = slot_entry(a, which ? SLOT_BRAKELIGHT : SLOT_HEADLIGHT);
		const u32 baseHash = e ? pe_light(e) : 0;
		if (!baseHash)
			continue;
		const u32* lights = which ? kBrakelightSlots : kHeadlightSlots;
		for (int i = 0; i < 4; i++) {
			// A part whose texture set lacks a state (the CORSA's HYDE
			// headlight has no _GLASS_OFF) keeps the slot's stock texture.
			add_override(a, lights[i], ext(baseHash, kLightSuffix[i]));
		}
	}
}

// UpdateDecalTextures (SPEED2.EXE 0x61FF40, Speed.elf 0x80234fd0). Every
// layout position starts at DEFAULTALPHA. Then each of the seven layouts the
// save fitted -- the hood's only on a car with a HOOD part -- that has both a
// SIZE and a SHAPE points position k at the decal in CarSlotId
// 0x62 + layout * 8 + k: its NAME hash carried on with "_SQUARE", "_RECT" or
// "_WIDE", by the layout's SHAPE. An empty position keeps DEFAULTALPHA.
static void choose_decals(Asm* a) {
	static const char* const shapeName[3] = { "SQUARE", "RECT", "WIDE" };
	static const char* const suffix[3] = { "_SQUARE", "_RECT", "_WIDE" };
	for (int d = 0; d < DECAL_LAYOUTS; d++) {
		const int s = SLOT_DECAL_HOOD + d;
		if (s >= a->numSlots || !a->slots[s])
			continue;
		if (s == SLOT_DECAL_HOOD && !(SLOT_HOOD < a->numSlots && a->slots[SLOT_HOOD]))
			continue;
		u32 shape;
		if (!pm_attr(a->pm, a->slots[s], &shape))
			continue;
		int si = 0;
		while (si < 3 && ext(NO_HASH, shapeName[si]) != shape)
			si++;
		// Retail would reuse whatever suffix its buffer last held; no shipped
		// layout has another SHAPE.
		if (si == 3)
			continue;
		for (int k = 0; k < DECAL_POSITIONS; k++) {
			const int t = SLOT_DECAL_TEX + d * DECAL_POSITIONS + k;
			if (t >= a->numSlots || !a->slots[t])
				continue;
			u32 name = 0;
			pm_attr(a->pm, a->slots[t], &name);
			a->decalTex[d][k] = ext(name, suffix[si]);
			a->nDecals++;
		}
	}
	if (a->nDecals)
		a->decalKit = get_kit_named(a, DECAL_KIT);
}

typedef struct {
	Kit* kit;
	int entry;
	u32 hash;
} TexRef;

static int tex_ref(TexRef* refs, int* n, Kit* k, u32 hash) {
	for (int i = 0; i < *n; i++)
		if (refs[i].hash == hash)
			return i;
	const int e = kit_tex(k, hash);
	if (e < 0 || *n >= MAX_TEX)
		return -1;
	refs[*n].kit = k;
	refs[*n].entry = e;
	refs[*n].hash = hash;
	return (*n)++;
}

static bool skin_slot(const Asm* a, u32 slot) {
	return slot == TEXSLOT_BODY_OUTER || slot == TEXSLOT_BODY_TRIM ||
	       slot == TEXSLOT_GLOBAL_SKIN1 || slot == TEXSLOT_SPOILER_SKIN ||
	       slot == TEXSLOT_ROOF_SKIN || slot == a->skinTex;
}

static s32 batch_tex(Asm* a, const Inst* in, u32 slot, TexRef* refs, int* nRefs) {
	if (in->slot >= SLOT_DECAL_HOOD && in->slot <= SLOT_LAST_MODEL &&
	    slot - TEXSLOT_DUMMY_DECAL1 < DECAL_POSITIONS) {
		const int d = in->slot <= SLOT_DECAL_RIGHT_QUARTER
			? in->slot - SLOT_DECAL_HOOD
			: in->slot - SLOT_WIDE_DECAL_LEFT_DOOR + (SLOT_DECAL_LEFT_DOOR - SLOT_DECAL_HOOD);
		const u32 th = a->decalTex[d][slot - TEXSLOT_DUMMY_DECAL1];
		const s32 r = (th && a->decalKit) ? tex_ref(refs, nRefs, a->decalKit, th) : -1;
		return r >= 0 ? r : TEX_HIDDEN;
	}
	if (a->skinTex && skin_slot(a, slot))
		return tex_ref(refs, nRefs, a->carKit, a->skinTex);
	u32 th = 0;
	if (in->kit == a->carKit)
		for (int i = 0; i < a->nOver; i++)
			if (a->overSlot[i] == slot)
				th = a->overTex[i];
	if (!th)
		th = kit_slot(in->kit, slot);
	return th ? tex_ref(refs, nRefs, in->kit, th) : -1;
}

// What carries a normal through `m` (its upper 3x3 A, row vectors): the
// inverse transpose of A, up to a positive scale the caller normalises away.
// That is A itself for every rotation and mirror an instance uses, but not
// for a rim stretched across its width, whose normals would otherwise lean
// toward the axis it was stretched along. Cofactors divided by the sign of
// det(A) -- not by det(A) itself, so the scale stays positive and a mirror's
// normals still come out facing the way its surface does.
static void normal_matrix(const float m[16], float n[9]) {
	const float a00 = m[0], a01 = m[1], a02 = m[2];
	const float a10 = m[4], a11 = m[5], a12 = m[6];
	const float a20 = m[8], a21 = m[9], a22 = m[10];
	n[0] = a11 * a22 - a12 * a21;
	n[1] = a12 * a20 - a10 * a22;
	n[2] = a10 * a21 - a11 * a20;
	n[3] = a02 * a21 - a01 * a22;
	n[4] = a00 * a22 - a02 * a20;
	n[5] = a01 * a20 - a00 * a21;
	n[6] = a01 * a12 - a02 * a11;
	n[7] = a02 * a10 - a00 * a12;
	n[8] = a00 * a11 - a01 * a10;
	if (a00 * n[0] + a01 * n[1] + a02 * n[2] < 0.0f)
		for (int i = 0; i < 9; i++)
			n[i] = -n[i];
}

static void wr32(u8* p, u32 v) { memcpy(p, &v, 4); }
static void wrf(u8* p, float v) { memcpy(p, &v, 4); }

// Writes the instances out as one .nfsc in memory.
// UpdateCarParts (Speed.elf 0x802334d0) takes the render box over GC model
// slots 0..7 and 0x2f..0x33 -- BASE..ROOF and the five damage slots here,
// one higher -- then pushes it out by 0.1 m (0x8033bdb4) on every side.
#define SHADOW_BOX_PAD 0.1f

static bool shadow_box_slot(int s) {
	return (s >= SLOT_BASE && s <= SLOT_ROOF) ||
	       (s >= SLOT_DAMAGE_FRONT && s < SLOT_DECAL_HOOD);
}

// The materials CarRenderInfo's constructor (Speed.elf 0x802314c0) looks up
// with elGetLightMaterial: the body paint part's LIGHT_MATERIAL (+0x384), each
// component paint's (+0x390..+0x3ac) under the same gate as its colour and
// else the body's, and the window tint's (+0x38c), WINDSHIELD without one.
static u32 part_light(const Asm* a, u32 part) {
	u32 v = 0;
	if (part)
		pm_attr(a->pm, part, &v);
	return v;
}

static void choose_light_mats(Asm* a, const CarKitInfo* info) {
	const u32 base = SLOT_BASE_PAINT < a->numSlots ? a->slots[SLOT_BASE_PAINT] : 0;
	a->paintMat[CARKIT_PAINT_BODY] = lightmat_get(part_light(a, base));
	for (int k = 1; k < CARKIT_PAINT_SLOTS; k++)
		a->paintMat[k] = info->paintPart[k]
			? lightmat_get(part_light(a, info->paintPart[k]))
			: a->paintMat[CARKIT_PAINT_BODY];
	const u32 tint = SLOT_WINDOW_TINT < a->numSlots
		? part_light(a, a->slots[SLOT_WINDOW_TINT]) : 0;
	a->tintMat = lightmat_get(tint ? tint : LM_WINDSHIELD);
}

// What Render lights a batch with: its own material `mh`, or the car's in
// its place. A rim or caliper under its component paint (PAINT_TINT) takes
// that paint's material too, as Render's wheel section makes the swap.
// PORT: the carbon-fibre flags (+0x3b0..+0x3c8) are not traced yet, so a
// carbon placeholder is always the body paint.
static const LightMat* batch_light_mat(const Asm* a, const CarKitInfo* info,
		const Inst* in, u32 mh, u32 fl) {
	const int ps = in->paint < CARKIT_PAINT_SLOTS ? in->paint : CARKIT_PAINT_BODY;
	switch (mh) {
	case LM_CARSKIN:    return a->paintMat[ps];
	case LM_WINDSHIELD: return a->tintMat;
	case LM_BODY:       return a->paintMat[CARKIT_PAINT_BODY];
	case LM_MIRROR:     return a->paintMat[CARKIT_PAINT_MIRROR];
	case LM_AUDIO:      return a->paintMat[CARKIT_PAINT_AUDIO];
	case LM_EXHAUST_TIP:
		if (info->paintPart[CARKIT_PAINT_EXHAUST])
			return a->paintMat[CARKIT_PAINT_EXHAUST];
		break;
	default:
		break;
	}
	if ((fl & MODEL3D_BATCH_PAINT_TINT) && info->paintPart[ps])
		return a->paintMat[ps];
	return lightmat_get(mh);
}

static u8* build_blob(Asm* a, size_t* outSize, CarKitInfo* info) {
	u32 nVert = 0, nIdx = 0, nBatch = 0;
	for (int i = 0; i < a->nInst; i++) {
		const u8* p = a->inst[i].part;
		nBatch += rd32(p + 8);
		nVert += rd32(p + 16);
		nIdx += rd32(p + 24);
	}
	if (!nVert || !nIdx || nVert > 65535) {
		alog(a, "carkit: %lu vertices -- cannot index with u16", (unsigned long)nVert);
		return NULL;
	}

	TexRef* refs = (TexRef*)malloc(MAX_TEX * sizeof(TexRef));
	s32* bTex = (s32*)malloc((nBatch ? nBatch : 1) * sizeof(s32));
	if (!refs || !bTex) {
		free(refs);
		free(bTex);
		return NULL;
	}
	int nRefs = 0;
	u32 bi = 0;
	free(a->glassBatch);
	free(a->glassWin);
	a->nGlass = 0;
	a->glassBatch = s_glass ? (int*)malloc((nBatch ? nBatch : 1) * sizeof(int)) : NULL;
	a->glassWin = s_glass ? (u8*)malloc(nBatch ? nBatch : 1) : NULL;
	for (int i = 0; i < a->nInst; i++) {
		const Inst* in = &a->inst[i];
		const u32 fb = rd32(in->part + 4), nb = rd32(in->part + 8);
		for (u32 j = 0; j < nb; j++, bi++) {
			const u8* b = in->kit->tab + in->kit->oBatch + (fb + j) * in->kit->batchSize;
			bTex[bi] = batch_tex(a, in, rd32(b + 8), refs, &nRefs);
			const int w = bTex[bi] >= 0 ? glass_window(rd32(b + 8)) : -1;
			if (w >= 0 && a->glassBatch && a->glassWin) {
				a->glassBatch[a->nGlass] = (int)bi;
				a->glassWin[a->nGlass++] = (u8)w;
			}
		}
	}
	// The cracked panes go in with the car's glass, from CARS/GLASS.nfsk.
	if (a->nGlass) {
		Kit* gk = get_kit_named(a, GLASS_KIT);
		for (int k = 0; k < 3 && a->nGlass; k++) {
			a->glassTex[k] = gk ? tex_ref(refs, &nRefs, gk, kGlassTex[k]) : -1;
			if (a->glassTex[k] < 0) {
				alog(a, "carkit: no %s in CARS/%s.nfsk -- %s's glass never cracks",
					k ? "cracked pane" : "WINDOW_DAMAGE0", GLASS_KIT, a->car);
				a->nGlass = 0;
			}
		}
	}
	u32 texBytes = 0;
	for (int t = 0; t < nRefs; t++)
		texBytes += kit_tex_size(refs[t].kit, refs[t].entry);

	const u32 oTex = NFSC_HDR;
	const u32 oBatch = oTex + (u32)nRefs * 16;
	const u32 oVert = oBatch + nBatch * 16;
	const u32 oIdx = oVert + nVert * NFSK_VERT;
	const u32 oTexBlob = oIdx + ((nIdx * 2 + 3) & ~3u);
	const size_t size = (size_t)oTexBlob + texBytes;
	u8* blob = (u8*)calloc(1, size);
	free(a->batchMat);
	a->batchMat = (blob && a->paintMat[CARKIT_PAINT_BODY] && nBatch)
		? (u8*)malloc(nBatch) : NULL;
	a->nBatchMat = a->batchMat ? nBatch : 0;
	if (!blob) {
		alog(a, "carkit: no memory for a %lu-byte car", (unsigned long)size);
		free(refs);
		free(bTex);
		return NULL;
	}

	// Every run the blob takes off the kits -- each instance's vertices and
	// indices, then the textures -- goes in one batch (kit_read_batch()).
	bool ok = true;
	{
		KRead* rs = (KRead*)malloc((size_t)(a->nInst * 2 + nRefs + 1) * sizeof(KRead));
		int nr = 0;
		u32 vb = 0, ib = 0;
		for (int i = 0; rs && i < a->nInst; i++) {
			const Inst* in = &a->inst[i];
			const Kit* k = in->kit;
			const u8* p = in->part;
			const u32 fv = rd32(p + 12), nv = rd32(p + 16), fi = rd32(p + 20), ni = rd32(p + 24);
			rs[nr++] = (KRead){ k, k->vertOff + fv * NFSK_VERT, nv * NFSK_VERT,
				blob + oVert + vb * NFSK_VERT };
			rs[nr++] = (KRead){ k, k->idxOff + fi * 2, ni * 2, (u8*)((u16*)(blob + oIdx) + ib) };
			vb += nv;
			ib += ni;
		}
		u32 to = 0;
		for (int t = 0; rs && t < nRefs; t++) {
			const Kit* k = refs[t].kit;
			const u32 sz = kit_tex_size(k, refs[t].entry);
			rs[nr++] = (KRead){ k, k->texOff + rd32(k->tab + refs[t].entry * NFSK_TEX + 12), sz,
				blob + oTexBlob + to };
			to += sz;
		}
		u32 kb = 0;
		for (int r = 0; r < nr; r++)
			kb += rs[r].size;
		s_blobReadKB = kb >> 10;
		const u64 t0 = osGetTime();
		ok = rs && kit_read_batch(rs, nr);
		s_blobReadMs = (u32)(osGetTime() - t0);
		free(rs);
	}

	float bbmin[3] = { 1e30f, 1e30f, 1e30f }, bbmax[3] = { -1e30f, -1e30f, -1e30f };
	float shmin[3] = { 1e30f, 1e30f, 1e30f }, shmax[3] = { -1e30f, -1e30f, -1e30f };
	u32 vBase = 0, iBase = 0;
	bi = 0;
	for (int i = 0; i < a->nInst && ok; i++) {
		const Inst* in = &a->inst[i];
		const Kit* k = in->kit;
		const u8* p = in->part;
		const u32 fb = rd32(p + 4), nb = rd32(p + 8), fv = rd32(p + 12), nv = rd32(p + 16);
		const u32 fi = rd32(p + 20), ni = rd32(p + 24);

		(void)fv;
		(void)fi;
		u8* vd = blob + oVert + vBase * NFSK_VERT;
		u16* id = (u16*)(blob + oIdx) + iBase;

		const float* M = in->m;
		float N[9];
		normal_matrix(M, N);
		// LOD A is the car the bounds and the render box are of.
		// A conditional part (door panels, undersides) is not the car's shape.
		const bool isA = inst_is_a(in) && !(in->partFl & MODEL3D_BATCH_PART_COND) &&
			in->slot != SLOT_ENGINE &&
			!(in->slot >= SLOT_TRUNK_AUDIO && in->slot <= SLOT_LAST_AUDIO);
		const bool inBox = isA && shadow_box_slot(in->slot);
		for (u32 v = 0; v < nv; v++) {
			float f[8];
			memcpy(f, vd + v * NFSK_VERT, sizeof(f));
			if (in->mirrorY) {
				f[1] = -f[1];
				f[4] = -f[4];
			}
			const float x = f[0], y = f[1], z = f[2];
			f[0] = x * M[0] + y * M[4] + z * M[8] + M[12];
			f[1] = x * M[1] + y * M[5] + z * M[9] + M[13];
			f[2] = x * M[2] + y * M[6] + z * M[10] + M[14];
			const float nx = f[3], ny = f[4], nz = f[5];
			float n0 = nx * N[0] + ny * N[3] + nz * N[6];
			float n1 = nx * N[1] + ny * N[4] + nz * N[7];
			float n2 = nx * N[2] + ny * N[5] + nz * N[8];
			const float len = sqrtf(n0 * n0 + n1 * n1 + n2 * n2);
			if (len > 1e-6f) {
				n0 /= len; n1 /= len; n2 /= len;
			}
			f[3] = n0; f[4] = n1; f[5] = n2;
			for (int c = 0; c < 3 && isA; c++) {
				if (f[c] < bbmin[c]) bbmin[c] = f[c];
				if (f[c] > bbmax[c]) bbmax[c] = f[c];
				if (inBox && f[c] < shmin[c]) shmin[c] = f[c];
				if (inBox && f[c] > shmax[c]) shmax[c] = f[c];
			}
			memcpy(vd + v * NFSK_VERT, f, sizeof(f));
		}
		for (u32 j = 0; j < ni; j++)
			id[j] = (u16)(id[j] + vBase);

		for (u32 j = 0; j < nb; j++, bi++) {
			const u8* b = k->tab + k->oBatch + (fb + j) * k->batchSize;
			u8* o = blob + oBatch + bi * 16;
			const u32 kf = rd32(b + 12);
			u32 fl = (kf & ~(KIT_MAT_MASK | MODEL3D_BATCH_LOD_MASK |
			                 MODEL3D_BATCH_CORNER_MASK | MODEL3D_BATCH_WHEEL_MASK |
			                 MODEL3D_BATCH_PART_COND | MODEL3D_BATCH_PART_LIT |
			                 MODEL3D_BATCH_PART_AUDIO)) |
			         ((u32)in->paint << 24) | ((u32)in->lods << MODEL3D_BATCH_LOD_SHIFT) |
			         ((u32)in->wheel << MODEL3D_BATCH_CORNER_SHIFT) | in->partFl;
			if (in->tint && ((kf & KIT_MAT_MASK) >> KIT_MAT_SHIFT) == in->tint)
				fl |= MODEL3D_BATCH_PAINT_TINT;
			if (a->batchMat)
				a->batchMat[bi] = lightmat_index(batch_light_mat(a, info, in,
					k->batchSize >= NFSK_BATCH ? rd32(b + 16) : 0, fl));
			const bool hidden = bTex[bi] == TEX_HIDDEN;
			wr32(o, iBase + rd32(b));
			wr32(o + 4, hidden ? 0 : rd32(b + 4));
			wr32(o + 8, (u32)(hidden ? -1 : bTex[bi]));
			wr32(o + 12, fl);
		}
		vBase += nv;
		iBase += ni;
	}

	u32 tOff = 0;
	for (int t = 0; t < nRefs && ok; t++) {
		const Kit* k = refs[t].kit;
		const u8* e = k->tab + refs[t].entry * NFSK_TEX;
		const u32 sz = kit_tex_size(k, refs[t].entry);
		u8* o = blob + oTex + t * 16;
		memcpy(o, e, 12);               // hash, w, h, fmt, mat
		wr32(o + 12, tOff);
		tOff += sz;
	}
	free(refs);
	free(bTex);
	if (!ok) {
		alog(a, "carkit: short read assembling %s", a->car);
		free(blob);
		return NULL;
	}

	wr32(blob + 0x00, NFSC_MAGIC);
	wr32(blob + 0x04, NFSC_VERSION);
	wr32(blob + 0x08, (u32)nRefs);
	wr32(blob + 0x0c, nBatch);
	wr32(blob + 0x10, nVert);
	wr32(blob + 0x14, nIdx);
	for (int c = 0; c < 3; c++) {
		wrf(blob + 0x18 + c * 4, bbmin[c]);
		wrf(blob + 0x24 + c * 4, bbmax[c]);
	}
	wr32(blob + 0x30, oTexBlob);
	wr32(blob + 0x34, texBytes);
	wr32(blob + 0x38, 0);  // SHADING_LIT
	wr32(blob + 0x3c, 0);  // no LOD split
	// A car with none of the box's slots (never seen) takes the whole model.
	const bool any = shmin[0] <= shmax[0];
	for (int c = 0; c < 3; c++) {
		info->shadowMin[c] = (any ? shmin[c] : bbmin[c]) - SHADOW_BOX_PAD;
		info->shadowMax[c] = (any ? shmax[c] : bbmax[c]) + SHADOW_BOX_PAD;
	}
	for (int i = 0; i < a->nInst; i++) {
		const int nv = (int)rd32(a->inst[i].part + 16);
		if (inst_is_a(&a->inst[i])) {
			info->parts++;
			info->verts += nv;
		} else {
			info->lodParts++;
			info->lodVerts += nv;
		}
	}
	info->textures = nRefs;
	*outSize = size;
	return blob;
}

// CompositeSkin's reads off the save (SPEED2.EXE 0x61D790): the base, the
// seven region colours, the four vinyl layers and their remap colours.
static void fill_skin(const Asm* a, const char* doorline, CarSkinDesc* d) {
	memset(d, 0, sizeof(*d));
	const unsigned int* slots = a->slots;
	const int n = a->numSlots;
	if (SLOT_BASE_PAINT < n)
		d->basePaint = slots[SLOT_BASE_PAINT];
	for (int i = 0; i < CARSKIN_REGIONS; i++)
		if (kSkinRegions[i] < n)
			d->regionPart[i] = slots[kSkinRegions[i]];
	// The audio region keeps the base under anything short of a level-3
	// trunk install.
	if (slot_level(a, SLOT_TRUNK_AUDIO) < 3)
		d->regionPart[SKIN_REGION_AUDIO] = 0;
	for (int k = 0; k < CARSKIN_VINYLS; k++) {
		if (SLOT_VINYL_LAYER0 + k < n)
			d->vinyl[k] = slots[SLOT_VINYL_LAYER0 + k];
		for (int j = 0; j < CARSKIN_REMAPS; j++) {
			const int s = SLOT_VINYL_COLOUR + k * CARSKIN_REMAPS + j;
			if (s < n)
				d->vinylColour[k][j] = slots[s];
		}
	}
	snprintf(d->doorline, sizeof(d->doorline), "%s", doorline);
	// CompositeRim / CompositeSpinner's: the front wheel part and the paints
	// at RideInfo +0x590 + 0x49 * 4 and + 0x4A * 4.
	if (SLOT_FRONT_WHEEL < n)
		d->frontWheel = slots[SLOT_FRONT_WHEEL];
	if (SLOT_PAINT_RIM < n)
		d->rimPaint = slots[SLOT_PAINT_RIM];
	if (SLOT_PAINT_SPINNER < n)
		d->spinnerPaint = slots[SLOT_PAINT_SPINNER];
}

// PARTMODELS.nfsm and an assembler for `car` wearing `slots`, with the car's
// own kit opened when it is on the card. NULL (and nothing to free) when the
// table is missing.
static Asm* asm_begin(const char* dataRoot, const char* car, const unsigned int* slots,
		int numSlots, PartModels* pm, void (*log)(const char* msg)) {
	if (!dataRoot || !car || !slots || numSlots <= SLOT_BASE)
		return NULL;
	if (s_kc.hold)
		kc_root(dataRoot);
	if (s_kc.hold && s_kc.pm.buf) {
		*pm = s_kc.pm;
	} else {
		char path[320];
		snprintf(path, sizeof(path), "%s/GLOBAL/PARTMODELS.nfsm", dataRoot);
		if (!pm_load(path, pm)) {
			if (log)
				log("carkit: no GLOBAL/PARTMODELS.nfsm -- stock car");
			return NULL;
		}
		if (s_kc.hold)
			s_kc.pm = *pm;   // the cache's from here, freed by kc_free
	}
	s_kc.hits = s_kc.misses = 0;
	Asm* a = (Asm*)calloc(1, sizeof(Asm));
	if (!a) {
		if (pm->buf != s_kc.pm.buf)
			free(pm->buf);
		return NULL;
	}
	a->root = dataRoot;
	a->car = car;
	a->pm = pm;
	a->slots = slots;
	a->numSlots = numSlots;
	a->log = log;
	enable_slots(a);
	a->carKit = get_kit_named(a, car);
	return a;
}

static void asm_end(Asm* a, PartModels* pm) {
	for (int i = 0; i < a->nKits; i++) {
		Kit* k = &a->kits[i];
		if (k->cached || kc_put(k))
			continue;   // the cache's file and table, or now handed to it
		kit_close(k);
	}
	free(a->batchMat);
	free(a->glassBatch);
	free(a->glassWin);
	free(a);
	if (pm->buf != s_kc.pm.buf)
		free(pm->buf);
}

bool carkit_lights(const char* dataRoot, const char* car,
		const unsigned int* slots, int numSlots, CarKitInfo* info,
		void (*log)(const char* msg)) {
	memset(info, 0, sizeof(*info));
	PartModels pm;
	Asm* a = asm_begin(dataRoot, car, slots, numSlots, &pm, log);
	if (!a)
		return false;
	const bool ok = a->carKit != NULL;
	if (ok) {
		collect_lights(a, info);
		collect_exhaust(a, info);
	}
	asm_end(a, &pm);
	return ok;
}

Model3D* carkit_assemble(const char* dataRoot, const char* car,
		const unsigned int* slots, int numSlots, CarKitInfo* info,
		void (*log)(const char* msg)) {
	CarKitInfo dummy;
	if (!info)
		info = &dummy;
	memset(info, 0, sizeof(*info));
	PartModels pm;
	// #402: where a rebuild's time goes, one line per assembly.
	u64 tm[9];
	tm[0] = osGetTime();
	Asm* a = asm_begin(dataRoot, car, slots, numSlots, &pm, log);
	if (!a)
		return NULL;
	tm[1] = tm[2] = tm[3] = tm[4] = tm[5] = tm[6] = tm[7] = tm[8] = osGetTime();

	Model3D* model = NULL;
	if (a->carKit) {
		char name[64];
		snprintf(name, sizeof(name), "%s_SKIN1", car);
		a->skinTex = ext(NO_HASH, name);
		if (kit_tex(a->carKit, a->skinTex) < 0) {
			alog(a, "carkit: CARS/%s.nfsk has no %s -- repack it with tools/kit_pack.py",
				car, name);
			a->skinTex = 0;
		}
		const char* doorline = "";
		choose_textures(a, &doorline);
		choose_decals(a);
		tm[2] = osGetTime();
		assemble_parts(a);
		fit_lods(a);
		tm[3] = osGetTime();
		collect_neon(a, info);
		collect_lights(a, info);
		collect_purge(a, info);
		collect_exhaust(a, info);
		collect_glass(a, info);
		collect_rims(a, info);
		for (size_t g = 0; g < sizeof(kPaintGates) / sizeof(kPaintGates[0]); g++) {
			const int ps = kPaintGates[g].paintSlot;
			if (slot_level(a, kPaintGates[g].gate) >= kPaintGates[g].minLevel &&
			    ps < numSlots)
				info->paintPart[kPaintGates[g].paint] = slots[ps];
		}
		tm[4] = osGetTime();
		if (lightmat_load(dataRoot))
			choose_light_mats(a, info);
		size_t size = 0;
		s_blobReadMs = s_blobReadKB = 0;
		u8* blob = a->nInst ? build_blob(a, &size, info) : NULL;
		tm[5] = osGetTime();
		if (blob) {
			// #436: the same art up for another live car is borrowed, not
			// uploaded again (the same car twice, the shared kits).
			u32 h0, kb0, h1, kb1, nLive, kbLive;
			model3d_share_stats(&h0, &kb0, NULL, NULL);
			model3d_share_textures(true);
			model = model3d_from_memory(blob, size);
			model3d_share_textures(false);
			free(blob);
			model3d_share_stats(&h1, &kb1, &nLive, &kbLive);
			if (model)
				alog(a, "carkit: %s shares %lu texture(s), %lu KB, with the cars already up "
					"(the share holds %lu, %lu KB)", car, (unsigned long)(h1 - h0),
					(unsigned long)(kb1 - kb0), (unsigned long)nLive, (unsigned long)kbLive);
		}
		tm[6] = osGetTime();
		if (model && a->bayLit)
			model3d_set_car_bay(model, true);
		if (model && a->rims == 4)
			model3d_set_car_pivots(model, (const float (*)[3])a->pivot);
		if (model && info->lodParts) {
			model3d_set_car_lods(model, true);
			// The rear rims' local +y, for TireFace in the world.
			if (info->rims == 4) {
				float axis[2][3];
				for (int j = 0; j < 2; j++)
					memcpy(axis[j], &info->rim[2 + j][4], sizeof(axis[j]));
				model3d_set_car_rims(model, (const float (*)[3])axis);
			}
		}
		if (model && a->batchMat) {
			if (model3d_set_light_mats(model, a->batchMat, (int)a->nBatchMat))
				alog(a, "carkit: %s lit as %s, windows %s (%lu batches)", car,
					a->paintMat[CARKIT_PAINT_BODY]->name, a->tintMat->name,
					(unsigned long)a->nBatchMat);
			else
				alog(a, "carkit: %s's %lu light materials not taken -- plain lighting",
					car, (unsigned long)a->nBatchMat);
		} else if (model) {
			alog(a, "carkit: no GLOBAL/LIGHTMATS.nfsl -- %s keeps the plain lighting", car);
		}
		// The glass, also per batch as built, before the merge (#324).
		if (model && a->nGlass) {
			if (model3d_set_glass(model, a->glassBatch, a->glassWin, a->nGlass, a->glassTex))
				alog(a, "carkit: %s's glass: %d batches can crack", car, a->nGlass);
			else
				alog(a, "carkit: %s's %d glass batches not taken -- it never cracks", car,
					a->nGlass);
		}
		collect_damage(a, info);
		// After the light materials, which are per batch as built (#345).
		if (model) {
			const int before = model3d_batch_count(model);
			const int gone = model3d_merge_batches(model);
			alog(a, "carkit: %s's %d batches drawn as %d", car, before, before - gone);
			alog(a, "carkit: %s opaque: %d batches in %d groups; %d states (%d without LOD, %d without retail bits); LOD A draws %d groups of %d states",
				car, g_mergeStat[0], g_mergeStat[1], g_mergeStat[2], g_mergeStat[3],
				g_mergeStat[4], g_mergeStat[5], g_mergeStat[6]);
			int deadG, deadE;
			model3d_dead_passes(model, &deadG, &deadE);
			alog(a, "carkit: %s: %d of %d batches add no gloss, %d no reflection -- not drawn there",
				car, deadG, model3d_batch_count(model), deadE);
		}
		tm[7] = osGetTime();
		fill_skin(a, doorline, &info->skin);
		tm[8] = osGetTime();
		alog(a, "carkit: %s from %d parts, %d verts (+%d LOD B-D parts, %d verts), %d textures, %d decals, %d neon tubes, %d lights%s, doorline%s%s",
			car, info->parts, info->verts, info->lodParts, info->lodVerts, info->textures,
			a->nDecals, info->neonSegs,
			info->lights, info->centreBrakeOff ? " (centre brake off)" : "",
			doorline[0] ? " " : " plain", doorline);
		if (a->nDecals && !a->decalKit)
			alog(a, "carkit: %d decals but no CARS/%s.nfsk -- none drawn", a->nDecals, DECAL_KIT);
	}
	asm_end(a, &pm);
	char tb[288];
	if (log)
		snprintf(tb, sizeof(tb), "carkit: %s times (ms): open %lu, textures %lu, parts %lu, collect %lu, blob %lu (read %lu, %lu KB), "
		"model %lu, merge %lu, skin %lu, close %lu; runs %lu cached, %lu read (%lu KB kept)", car,
		(unsigned long)(tm[1] - tm[0]), (unsigned long)(tm[2] - tm[1]),
		(unsigned long)(tm[3] - tm[2]), (unsigned long)(tm[4] - tm[3]),
		(unsigned long)(tm[5] - tm[4]), (unsigned long)s_blobReadMs,
		(unsigned long)s_blobReadKB, (unsigned long)(tm[6] - tm[5]),
		(unsigned long)(tm[7] - tm[6]), (unsigned long)(tm[8] - tm[7]),
		(unsigned long)(osGetTime() - tm[8]), (unsigned long)s_kc.hits,
		(unsigned long)s_kc.misses, (unsigned long)(s_kc.bytes >> 10));
	if (log)
		log(tb);
	return model;
}
