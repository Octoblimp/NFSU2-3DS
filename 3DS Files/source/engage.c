#include "engage.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "texpack.h"

// See engage.h for where every number below comes from.

// Uppercase-folded bStringHash, the key both the texture packs and the
// engine's widget lookup use.
#define EG_HASH_BACKING   0x99F665F4u // ENGAGE_BACKING,        128x128 DXT3
#define EG_HASH_ROSETTE   0xB996A84Cu // ENGAGE_DPAD_BACKING,     64x64 DXT3
#define EG_HASH_CINGULAR  0xEA570722u // ENGAGE_CINGULAR_ICON,    32x32 DXT3
#define EG_HASH_HIGHLIGHT 0xC3F846B5u // ENGAGE_DPAD_HIGHLIGHT,    16x8 DXT3
#define EG_HASH_MAIL      0x4B9B8F5Au // ENGAGE_SMS_MAIL,         64x32 DXT3
#define EG_HASH_PHONE     0x6FFF0767u // ENGAGE_PHONE_ICON,       32x32 DXT3

typedef enum {
	EG_BACKING, EG_ROSETTE, EG_CINGULAR, EG_HIGHLIGHT, EG_MAIL, EG_PHONE,
	EG_COUNT
} EngageTex;

// GetEventTypeTexHash (Speed.elf 0x801115F0), the whole switch, indexed by
// EventIndicatorType. 0 is not a type -- the engine's default arm returns a
// zero hash -- and types 5/6 and 9/10 deliberately share one texture, as the
// switch's own fallthrough does.
//
// Eighteen of the nineteen are in GLOBALB.BUN's Global\GlobalTextures.tpk, all
// 128x64, and their names there are the ones below -- which is the table's own
// cross-check: every hash the switch yields resolves to a texture whose name
// is the event type it was decoded as. Only type 19 is in neither pack this
// opens; it stays unloaded, and a type with no texture draws no tab.
#define EG_EVENT_TYPE_COUNT 20

static const u32 kEventTexHash[EG_EVENT_TYPE_COUNT] = {
	0x00000000u, // 0  not a type
	0xE9638D3Eu, // 1  MODE_ICON_CIRCUIT       (also the default arm's value)
	0xAAAB31E9u, // 2  MODE_ICON_DRAG
	0x00119144u, // 3  MODE_ICON_DRIFT
	0x2521E5EBu, // 4  MODE_ICON_SPRINT
	0x91703AEFu, // 5  MODE_ICON_SHORT_TRACK
	0x91703AEFu, // 6  MODE_ICON_SHORT_TRACK   -- shares 5's art, as the switch does
	0x3F6495C5u, // 7  MODE_ICON_U_RACING_LEAGUE
	0xBF5AC5A9u, // 8  MODE_ICON_SUV
	0x33B5791Au, // 9  ICON_RANDOM_ENCOUNTER
	0x33B5791Au, // 10 ICON_RANDOM_ENCOUNTER   -- shares 9's art
	0x8244F164u, // 11 SHOP_ICON_CAR_LOT
	0xB8038AC0u, // 12 SHOP_ICON_CRIB
	0xB802F10Eu, // 13 SHOP_ICON_BODY
	0xC56D3032u, // 14 SHOP_ICON_PERFORMANCE
	0xDC99AAFEu, // 15 SHOP_ICON_GRAPHIC
	0x3F40E75Fu, // 16 SHOP_ICON_INTERIORS
	0xD1D9D8A4u, // 17 ICON_SHOWCASE
	0xB0C0DCB5u, // 18 MODE_ICON_WORLD_CHALLENGE
	0xF95DD41Eu, // 19 not in GLOBALB.BUN
};

// _get_shop_indicator_type (Speed.elf 0x80111CA4), indexed by the shop's kind
// byte. Each pairing is confirmed twice over -- see eventdb.h -- by the shop
// record's own name and by the texture name the type resolves to above:
//
//   kind 0 -> 12  SHOP_ICON_CRIB          the safe house
//   kind 1 -> 15  SHOP_ICON_GRAPHIC       CC_GRAPHICS
//   kind 2 -> 13  SHOP_ICON_BODY          UC_BODYSHOP, SH_BODYSHOP
//   kind 3 -> 14  SHOP_ICON_PERFORMANCE   UC_PERF_SHOP
//   kind 4 -> 11  SHOP_ICON_CAR_LOT       UC_CAR_LOT
//   kind 5 -> 16  SHOP_ICON_INTERIORS     UC_CAR_SPECIALTY, CC_CAR_SPECIALTY
#define EG_SHOP_KIND_COUNT 6
static const u8 kShopIndicatorType[EG_SHOP_KIND_COUNT] = { 12, 15, 13, 14, 11, 16 };

// hud.c's HUD_SCALE: the 640x480 canvas at half size on a 400x240 screen.
#define EG_SCALE 0.5f

// Draw depth. One value for all of it, so the order of the calls in
// engage_draw() is the order on screen, which is the .fng's own posDepth order
// read far-to-near: panel 64, rosette 63, envelope 38, arrows 13, centre -27.
#define EG_DEPTH 0.5f

// The widgets, as {centre x, centre y, w, h} on the canvas. Each one is its
// guid's position summed down the PA chain from group 904267 at
// (-247.125, -121.625), and its authored size. Engage_SMS_mail has no PA, so
// its authored x is already absolute.
static const float kRect[EG_COUNT][4] = {
	{ -230.125f, -138.625f, 128.0f, 128.0f }, // Engage_Backing,        guid 903853
	{ -253.125f, -111.625f,  64.0f,  64.0f }, // Engage_DPAD_Backing,   guid 903864
	{ -253.125f, -110.625f,  32.0f,  32.0f }, // the rosette centre,    guid 904700
	{ -228.000f, -112.000f,  16.0f,   8.0f }, // Engage_DPAD_Highlight, guid 902460
	{ -195.000f, -112.000f,  64.0f,  32.0f }, // Engage_SMS_mail,       guid 919770
	{ -253.125f, -110.625f,  32.0f,  32.0f }, // the centre again, with UpdateGeneric's art
};

// The up arrow, guid 902463: the same Engage_DPAD_Highlight art as guid 902460
// -- both resolve to the screen's resource index 32 -- at its own position and
// without 902460's quarter turn. Its own slot in kRect would mean loading the
// texture twice, so it is a rectangle rather than an EngageTex: at
// (-253.0, -137.0), 16x8 -- kArrowAt's up entry below.

// PORT (#447): the four arrows' centres -- up and right the two authored ones
// above, down and left those mirrored through the rosette's centre
// (-253.125, -111.625) -- and the turn that points the art that way.
static const float kArrowAt[4][2] = {
	{ -253.0f,   -137.0f   }, // up, guid 902463
	{ -228.0f,   -112.0f   }, // right, guid 902460
	{ -253.25f,  -86.25f   }, // down
	{ -278.25f,  -111.25f  }, // left
};
static int s_shopDir = 0, s_smsDir = 1;

void engage_set_dirs(int shopDir, int smsDir) {
	s_shopDir = shopDir;
	s_smsDir = smsDir;
}

// The tab, guid 900478: authored at (17.0, -48.0) under group 904267, so
// (-247.125 + 17.0, -121.625 - 48.0) on the canvas, at its authored 114x57.
// It sits directly above the up arrow, which is what makes the pair read as
// "press Up for this".
static const float kRectTab[4] = { -230.125f, -169.625f, 114.0f, 57.0f };

// The SA blobs' colour tracks, read B,G,R,A the way fng.h documents.
#define EG_TINT_PANEL   C2D_Color32(255, 255, 255, 100) // guid 903853
#define EG_TINT_ROSETTE C2D_Color32(255, 255, 255, 200) // guid 903864
// 0x30,0x80,0xCF,0xFF in the file: blue 48, green 128, red 207 -- Cingular's
// orange. Shared by the rosette centre (904700) and the envelope (919770).
#define EG_TINT_ORANGE  C2D_Color32(207, 128,  48, 255)
// The right arrow is authored white at alpha 80. UpdateSMS's steady state
// gives it "static", whose colour track (48,193,332,255) has no keys and a red
// channel past 255; how FEng narrows that is not known, so the SMS arrow stays
// drawn white at full alpha. An unlit arrow is not drawn at all, which is what
// FEngSetInvisible does to it.
#define EG_TINT_ARROW   C2D_Color32(255, 255, 255, 255)

// The three scripts EngageEvent::UpdateGeneric attaches while an event is live
// (SPEED2.EXE 0x4C8BF2..0x4C8C5B; Speed.elf 0x80110EA8..0x80110EEC), decoded
// out of HUD_SingleRace.fng, base + delta per key. Script names hash without
// case, so the code's "pulse" on the arrow and "Pulse" on the centre are one
// script, PULSE (0x03826A28); "Blink" is BLINK (0x0280164F). All three run
// 800 ms and loop (Sh flags 1), and SetScript only starts one that is not
// already running, so they start together when the event does and keep going.
#define EG_LOOP_MS 800.0f
// The centre, guid 904700: 32x32 at alpha 255 for 380 ms, a 20 ms drop, then
// 28x28 at alpha 150 until the loop comes round. Its colour stays orange.
static const float kPulseMs[4] = { 0.0f, 380.0f, 400.0f, 800.0f };
static const float kPulseAlpha[4] = { 255.0f, 255.0f, 150.0f, 150.0f };
static const float kPulseSize[4] = { 32.0f, 32.0f, 28.0f, 28.0f };
// The up arrow, guid 902463: orange at alpha 255, 16x8 -> 26x14 -> 16x8.
// The tab, guid 900478: white, alpha 255 -> 180 -> 255, 114x57 -> 90x45 ->
// 114x57. Both on keys at 0, 400 and 800 ms.
static const float kKeyMs[3] = { 0.0f, 400.0f, 800.0f };
static const float kArrowW[3] = { 16.0f, 26.0f, 16.0f };
static const float kArrowH[3] = { 8.0f, 14.0f, 8.0f };
static const float kBlinkAlpha[3] = { 255.0f, 180.0f, 255.0f };
static const float kBlinkW[3] = { 114.0f, 90.0f, 114.0f };
static const float kBlinkH[3] = { 57.0f, 45.0f, 57.0f };

struct Engage {
	C3D_Tex tex[EG_COUNT];
	Tex3DS_SubTexture sub[EG_COUNT];
	C2D_Image img[EG_COUNT];
	bool texValid[EG_COUNT];
	// The tab's art, one slot per EventIndicatorType. Sparse on purpose: only
	// the types whose texture is in a pack this opens get a slot filled, and
	// the two shared-art pairs decode their texture twice rather than
	// special-casing them.
	C3D_Tex evTex[EG_EVENT_TYPE_COUNT];
	Tex3DS_SubTexture evSub[EG_EVENT_TYPE_COUNT];
	C2D_Image evImg[EG_EVENT_TYPE_COUNT];
	bool evValid[EG_EVENT_TYPE_COUNT];
	// EngageEvent +4: when the live event started (Start, 0x801109C0), which
	// is where the three scripts above are measured from.
	bool engaged;
	u64 startMs;
	// The prologue's phone (type 0x14): its slot's state, 0 with none, and
	// when it last entered 2 (#274).
	int phoneState;
	u64 phoneMs;
};

void engage_set_stage0_phone(Engage* e, int slotState) {
	if (!e)
		return;
	if (slotState == 2 && e->phoneState != 2)
		e->phoneMs = osGetTime();
	e->phoneState = slotState;
}

int engage_shop_indicator_type(int shopKind) {
	if (shopKind >= 0 && shopKind < EG_SHOP_KIND_COUNT)
		return (int)kShopIndicatorType[shopKind];
	return 1; // 0x80111CA4's default arm
}

// Canvas (centre-origin, y down) -> top screen, left/top anchored.
static float egScreenX(float cx) { return (cx + 320.0f) * EG_SCALE; }
static float egScreenY(float cy) { return (cy + 240.0f) * EG_SCALE; }

void engage_key_anchor(int dir, float* x, float* y) {
	if (dir < 0 || dir > 3)
		dir = 0;
	*x = egScreenX(kArrowAt[dir][0]);
	*y = egScreenY(kArrowAt[dir][1]);
}

// The tab's art, from GLOBAL/GLOBALB.BUN. Separate from the five above because
// it is a different pack: HudTexturesSingleRace.tpk holds the panel and the
// rosette, GlobalTextures.tpk holds every MODE_ICON_* and SHOP_ICON_*.
static void egLoadEventIcons(Engage* e, const char* globalBunPath,
	void (*log)(const char*))
{
	char b[700];
	TexPack pack;
	if (!texpack_load_shared(globalBunPath, &pack)) {
		if (log) {
			snprintf(b, sizeof(b), "engage: %s would not load (texpack stage %d) "
				"-- no tab icons", globalBunPath, g_texpackFail.stage);
			log(b);
		}
		return;
	}
	int got = 0;
	for (int t = 1; t < EG_EVENT_TYPE_COUNT; t++) {
		const TexPackEntry* ent = texpack_find(&pack, kEventTexHash[t]);
		// maxDim 0: they are 128x64 and the tab draws them at 114x57.
		if (ent && texpack_decode_entry(ent, 0, false, &e->evTex[t], &e->evSub[t])) {
			e->evValid[t] = true;
			e->evImg[t].tex = &e->evTex[t];
			e->evImg[t].subtex = &e->evSub[t];
			got++;
		}
	}
	texpack_release(&pack);
	if (log) {
		snprintf(b, sizeof(b), "engage: %d/%d tab icons from %s",
			got, EG_EVENT_TYPE_COUNT - 1, globalBunPath);
		log(b);
	}
}

Engage* engage_open(const char* freeRoamBunPath, const char* globalBunPath,
	void (*log)(const char*))
{
	Engage* e = (Engage*)calloc(1, sizeof(Engage));
	if (!e)
		return NULL;

	if (globalBunPath)
		egLoadEventIcons(e, globalBunPath, log);
	if (!freeRoamBunPath)
		return e;

	static const u32 kHash[EG_COUNT] = {
		EG_HASH_BACKING, EG_HASH_ROSETTE, EG_HASH_CINGULAR,
		EG_HASH_HIGHLIGHT, EG_HASH_MAIL, EG_HASH_PHONE,
	};

	char b[700];
	TexPack pack;
	if (!texpack_load_shared(freeRoamBunPath, &pack)) {
		if (log) {
			snprintf(b, sizeof(b), "engage: %s would not load (texpack stage %d)",
				freeRoamBunPath, g_texpackFail.stage);
			log(b);
		}
		return e;
	}
	int got = 0;
	for (int i = 0; i < EG_COUNT; i++) {
		const TexPackEntry* ent = texpack_find(&pack, kHash[i]);
		// maxDim 0: the largest of the six is 128x128, and every one of them is
		// drawn at half its texels anyway.
		if (ent && texpack_decode_entry(ent, 0, false, &e->tex[i], &e->sub[i])) {
			e->texValid[i] = true;
			e->img[i].tex = &e->tex[i];
			e->img[i].subtex = &e->sub[i];
			got++;
		} else if (log) {
			snprintf(b, sizeof(b), "engage: texture 0x%08lx not in %s",
				(unsigned long)kHash[i], freeRoamBunPath);
			log(b);
		}
	}
	texpack_release(&pack);

	if (log) {
		snprintf(b, sizeof(b), "engage: %d/%d textures (panel %s, rosette %s, "
			"cingular %s, arrow %s, envelope %s, phone %s)",
			got, EG_COUNT,
			e->texValid[EG_BACKING]   ? "ok" : "MISSING",
			e->texValid[EG_ROSETTE]   ? "ok" : "MISSING",
			e->texValid[EG_CINGULAR]  ? "ok" : "MISSING",
			e->texValid[EG_HIGHLIGHT] ? "ok" : "MISSING",
			e->texValid[EG_MAIL]      ? "ok" : "MISSING",
			e->texValid[EG_PHONE]     ? "ok" : "MISSING");
		log(b);
	}
	return e;
}

void engage_close(Engage* e) {
	if (!e)
		return;
	for (int i = 0; i < EG_COUNT; i++) {
		if (e->texValid[i])
			C3D_TexDelete(&e->tex[i]);
	}
	for (int t = 0; t < EG_EVENT_TYPE_COUNT; t++) {
		if (e->evValid[t])
			C3D_TexDelete(&e->evTex[t]);
	}
	free(e);
}

// Draws slot `i` at `rect`'s canvas rectangle, tinted `tint`. `rot` is the
// widget's own rotation in radians, clockwise on this y-down screen, which is
// what the .fng's quaternion about +z means.
static void egDraw(Engage* e, int i, const float* rect, u32 tint, float rot) {
	if (!e->texValid[i])
		return;
	const float w = rect[2] * EG_SCALE;
	const float h = rect[3] * EG_SCALE;
	const float sx = w / (float)e->sub[i].width;
	const float sy = h / (float)e->sub[i].height;
	C2D_ImageTint t;
	C2D_PlainImageTint(&t, tint, 1.0f);
	if (rot != 0.0f) {
		C2D_DrawImageAtRotated(e->img[i], egScreenX(rect[0]), egScreenY(rect[1]),
			EG_DEPTH, rot, &t, sx, sy);
	} else {
		C2D_DrawImageAt(e->img[i], egScreenX(rect[0]) - w * 0.5f,
			egScreenY(rect[1]) - h * 0.5f, EG_DEPTH, &t, sx, sy);
	}
}

// One keytrack at `ms`: linear between keys, held past the last. hud.c's
// scriptTrack, for the same .fng scripts.
static float egTrack(const float* keyMs, const float* val, int n, float ms) {
	if (ms <= keyMs[0])
		return val[0];
	for (int i = 1; i < n; i++) {
		if (ms <= keyMs[i]) {
			const float f = (ms - keyMs[i - 1]) / (keyMs[i] - keyMs[i - 1]);
			return val[i - 1] + (val[i] - val[i - 1]) * f;
		}
	}
	return val[n - 1];
}

// The tab, drawn from its own texture array rather than kRect's slots, with
// BLINK at `ms` into its loop.
static void egDrawTab(Engage* e, int indicatorType, float ms) {
	if (indicatorType <= 0 || indicatorType >= EG_EVENT_TYPE_COUNT)
		return;
	if (!e->evValid[indicatorType])
		return;
	const float w = egTrack(kKeyMs, kBlinkW, 3, ms) * EG_SCALE;
	const float h = egTrack(kKeyMs, kBlinkH, 3, ms) * EG_SCALE;
	C2D_ImageTint t;
	C2D_PlainImageTint(&t,
		C2D_Color32(255, 255, 255, (u8)egTrack(kKeyMs, kBlinkAlpha, 3, ms)), 1.0f);
	C2D_DrawImageAt(e->evImg[indicatorType], egScreenX(kRectTab[0]) - w * 0.5f,
		egScreenY(kRectTab[1]) - h * 0.5f, EG_DEPTH, &t,
		w / (float)e->evSub[indicatorType].width,
		h / (float)e->evSub[indicatorType].height);
}

void engage_draw(Engage* e, bool smsWaiting, bool canEngage, int indicatorType) {
	if (!e)
		return;

	// Start (0x801109C0) stamps the event the frame it goes live; Finish
	// (0x80110A28) ends it the frame the car leaves the trigger.
	const u64 nowMs = osGetTime();
	if (canEngage && !e->engaged) {
		e->engaged = true;
		e->startMs = nowMs;
	} else if (!canEngage) {
		e->engaged = false;
	}
	const float ms = e->engaged ? fmodf((float)(nowMs - e->startMs), EG_LOOP_MS) : 0.0f;

	// The art carries its own colour -- Engage_Backing is black, the rosette is
	// a dark ring with grey arrows -- and the engine's vertex colour multiplies
	// it. citro2d's default C2D_TintSolid replaces the texture's RGB with the
	// tint instead, which turns the black panel white. hud.c's image path makes
	// the same switch for the same reason.
	C2D_SetTintMode(C2D_TintMult);

	egDraw(e, EG_BACKING, kRect[EG_BACKING], EG_TINT_PANEL, 0.0f);
	egDraw(e, EG_ROSETTE, kRect[EG_ROSETTE], EG_TINT_ROSETTE, 0.0f);
	if (canEngage) {
		// EngageEvent::UpdateGeneric, in every state the event is live in (2,
		// 3 and 4): ENGAGE_PHONE_ICON in the centre, pulsing (SPEED2.EXE
		// 0x4C8BB2 and 0x4C8C6A). The event outranks the SMS one (priority 4
		// against 2, GetEventPriority 0x80111768) and EventIndicator::Update
		// runs the live event last, so the phone holds with mail waiting too.
		const float s = egTrack(kPulseMs, kPulseSize, 4, ms);
		const float rect[4] = { kRect[EG_PHONE][0], kRect[EG_PHONE][1], s, s };
		egDraw(e, EG_PHONE, rect,
			C2D_Color32(207, 128, 48, (u8)egTrack(kPulseMs, kPulseAlpha, 4, ms)), 0.0f);
	} else if (e->phoneState >= 2 && e->phoneState <= 4 && !smsWaiting) {
		// EngageEvent::UpdateCareerStage0 (0x80110D40), the prologue's phone:
		// ENGAGE_PHONE_ICON (0x6FFF0767) on "Pulse" while the event arrives
		// (state 2), on "Init" -- the centre's authored 32x32, alpha 255 --
		// from 3 on. It lights no arrow and has no tab: the phone is the
		// whole of it.
		if (e->phoneState == 2) {
			const float pm = fmodf((float)(nowMs - e->phoneMs), EG_LOOP_MS);
			const float s = egTrack(kPulseMs, kPulseSize, 4, pm);
			const float rect[4] = { kRect[EG_PHONE][0], kRect[EG_PHONE][1], s, s };
			egDraw(e, EG_PHONE, rect,
				C2D_Color32(207, 128, 48, (u8)egTrack(kPulseMs, kPulseAlpha, 4, pm)), 0.0f);
		} else {
			egDraw(e, EG_PHONE, kRect[EG_PHONE], C2D_Color32(207, 128, 48, 255), 0.0f);
		}
	} else {
		// Engage_Cingular_Icon otherwise -- EventIndicator::SetResetState
		// (0x80111988) with nothing to show, UpdateSMS's steady state
		// (0x80111200, 0x801112ac) with mail waiting, and Finish's "Init" as an
		// event ends -- all at the centre's authored 32x32, alpha 255.
		egDraw(e, EG_CINGULAR, kRect[EG_CINGULAR], EG_TINT_ORANGE, 0.0f);
	}

	if (smsWaiting) {
		// UpdateSMS state 4: the envelope, and the right arrow lit between it
		// and the rosette.
		egDraw(e, EG_MAIL, kRect[EG_MAIL], EG_TINT_ORANGE, 0.0f);
		// guid 902460 carries the quaternion (0, 0, 0.7071, 0.7071) -- a
		// quarter turn about +z, which on a y-down canvas points the arrow
		// right. Its twin, guid 902463, is the same art unrotated, pointing up.
		// PORT (#447): the arrow of the direction the SMS is bound to.
		if (s_smsDir >= 0 && s_smsDir <= 3) {
			const float rect[4] = { kArrowAt[s_smsDir][0], kArrowAt[s_smsDir][1],
				kRect[EG_HIGHLIGHT][2], kRect[EG_HIGHLIGHT][3] };
			egDraw(e, EG_HIGHLIGHT, rect, EG_TINT_ARROW, (float)M_PI * 0.5f * (float)s_smsDir);
		}
	}

	if (canEngage) {
		// EngageEvent::UpdateGeneric (0x80110E0C): the up arrow lit, directly
		// under the tab, on PULSE.
		// PORT (#447): the arrow of the direction Enter Shop is bound to.
		if (s_shopDir >= 0 && s_shopDir <= 3) {
			const float rect[4] = { kArrowAt[s_shopDir][0], kArrowAt[s_shopDir][1],
				egTrack(kKeyMs, kArrowW, 3, ms), egTrack(kKeyMs, kArrowH, 3, ms) };
			egDraw(e, EG_HIGHLIGHT, rect, EG_TINT_ORANGE, (float)M_PI * 0.5f * (float)s_shopDir);
		}
	}
	// The tab carries the event's own icon, on BLINK. A shop resolves to one
	// through its kind byte (see the tables above), a race start through its
	// career event; one that resolves to neither draws the arrow alone rather
	// than a stand-in.
	egDrawTab(e, indicatorType, ms);

	C2D_SetTintMode(C2D_TintSolid);
}

// ---------------------------------------------------------------- the events

// GetEventPriority (0x80111768): race and shop types 4, 9 and 10 3, an SMS 2,
// the prologue's phone 1, anything else 0.
static int egPriority(int type) {
	if (type >= 1 && type <= 8) return 4;
	if (type == 9 || type == 10) return 3;
	if (type >= 0xB && type <= 0x12) return 4;
	if (type == ENGAGE_EVENT_SMS) return 2;
	if (type == ENGAGE_EVENT_STAGE0) return 1;
	return 0;
}

// EngageEvent::GetPriority (0x80110AB4): an empty slot has none.
static int egSlotPriority(const EngageEvent* ev) {
	return ev->state ? egPriority(ev->type) : 0;
}

// EngageEvent::playSound (0x80110C88). The prologue's phone never calls it.
static void egPlaySound(const EngageEvents* q, const EngageEvent* ev, int state) {
	if (!q->sound || ev->type == ENGAGE_EVENT_STAGE0) return;
	if (state == 2)
		q->sound(ev->type == ENGAGE_EVENT_SMS ? ENGAGE_SND_ARRIVE_SMS : ENGAGE_SND_ARRIVE);
	else if (state == 3)
		q->sound(ENGAGE_SND_ARRIVE_LATE);
}

// EngageEvent::Start (0x801109C0).
static void egStart(EngageEvents* q, EngageEvent* ev) {
	ev->state = 2;
	ev->start = q->now;
	egPlaySound(q, ev, 2);
}

// EngageEvent::updateState (0x80110AF0). The 2 -> 3 wait is 1 s, or the
// prologue phone's count; 3 -> 4 is 4 s from Start.
static void egUpdateState(EngageEvents* q, EngageEvent* ev, bool stage0Control) {
	const float t = q->now - ev->start;
	if (ev->state == 2) {
		const float wait = ev->type == ENGAGE_EVENT_STAGE0 ? (float)ev->count : 1.0f;
		if (ev->type == ENGAGE_EVENT_SMS && stage0Control)
			return;
		if (!(t > wait))
			return;
		egPlaySound(q, ev, 3);
		ev->state = 3;
	} else if (ev->state == 3) {
		if (!(t > 4.0f))
			return;
		ev->state = 4;
	}
}

// EngageEvent::Update (0x80111538), its state half: a slot with no type is
// empty. The per-type halves (UpdateGeneric, UpdateSMS...) are the art's.
static void egUpdate(EngageEvents* q, EngageEvent* ev, bool stage0Control) {
	if (ev->type == 0)
		ev->state = 0;
	if ((unsigned char)ev->state > 1)
		egUpdateState(q, ev, stage0Control);
}

// EngageEvent::Finish (0x80110A28): through 6, which only the art reads,
// back to empty.
static void egFinish(EngageEvent* ev) {
	ev->type = 0;
	ev->state = 0;
}

void engage_events_reset(EngageEvents* q, void (*sound)(int trigger)) {
	memset(q, 0, sizeof(*q));
	q->sound = sound;
}

// EventIndicator::AddEvent (0x80111B34). A type already up (FindSameEvent
// 0x80111AFC: same type, not empty) arrives again only once it is shown --
// Restart (0x80110A00) is Start, sound and all -- and one still arriving
// keeps its count. With no free slot it is dropped.
void engage_events_add(EngageEvents* q, int type, int count) {
	for (int i = 0; i < ENGAGE_EVENT_SLOTS; i++) {
		EngageEvent* ev = &q->ev[i];
		if (ev->type == (signed char)type && ev->state != 0) {
			if (ev->state == 4) {
				egStart(q, ev);
				ev->count = count;
			}
			return;
		}
	}
	for (int i = 0; i < ENGAGE_EVENT_SLOTS; i++) {
		EngageEvent* ev = &q->ev[i];
		if (ev->state == 0) {
			ev->type = (signed char)type;   // Fill (0x80110990)
			ev->state = 1;
			ev->count = count;
			return;
		}
	}
}

// EventIndicator::RemoveEvent (0x80111BD0): the first slot of that type,
// whatever its state, Finishes.
void engage_events_remove(EngageEvents* q, int type) {
	for (int i = 0; i < ENGAGE_EVENT_SLOTS; i++) {
		if (q->ev[i].type == (signed char)type) {
			egFinish(&q->ev[i]);
			return;
		}
	}
}

// SMSMessageManager::Update (0x80113978). A count that falls without
// reaching zero is not remembered, so only a rise past the highest seen
// arrives again.
void engage_events_sms(EngageEvents* q, int unread) {
	if (unread == 0) {
		engage_events_remove(q, ENGAGE_EVENT_SMS);
		q->smsSeen = 0;
	} else if (q->smsSeen < unread) {
		engage_events_add(q, ENGAGE_EVENT_SMS, unread);
		q->smsSeen = unread;
	}
}

int engage_events_state(const EngageEvents* q, int type) {
	for (int i = 0; i < ENGAGE_EVENT_SLOTS; i++)
		if (q->ev[i].type == (signed char)type && q->ev[i].state)
			return q->ev[i].state;
	return 0;
}

void engage_events_update(EngageEvents* q, float dt, bool stage0Control) {
	q->now += dt;
	for (int i = 0; i < ENGAGE_EVENT_SLOTS; i++)
		egUpdate(q, &q->ev[i], stage0Control);
	// FindAnyEventInProgress (0x8011195C): the first slot in 2 or 3.
	// FindHighPriorityEvent (0x801118F8): the first of the highest.
	EngageEvent* live = NULL;
	EngageEvent* best = NULL;
	int bestPri = 0;
	for (int i = 0; i < ENGAGE_EVENT_SLOTS; i++) {
		EngageEvent* ev = &q->ev[i];
		if (!live && (ev->state == 2 || ev->state == 3))
			live = ev;
		const int p = egSlotPriority(ev);
		if (p > bestPri) {
			bestPri = p;
			best = ev;
		}
	}
	if (!live) {
		if (!best)
			return;          // SetResetState, the art's
		if (best->state != 4)
			egStart(q, best);
		egUpdate(q, best, stage0Control);
	} else if (!best || egSlotPriority(live) >= egSlotPriority(best)) {
		egUpdate(q, live, stage0Control);
	} else {
		// Overide (0x80110A6C): an SMS steps back to shown, anything else
		// Finishes. Then the higher one arrives.
		if (live->type == ENGAGE_EVENT_SMS) {
			live->state = 4;
			egUpdate(q, live, stage0Control);
		} else {
			egFinish(live);
		}
		egStart(q, best);
	}
}
