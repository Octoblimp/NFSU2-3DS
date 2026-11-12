#include "minimap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "texpack.h"

// See minimap.h for where every number below comes from.

// bStringHash (seed 0xFFFFFFFF, h = c + h * 0x21) of the texture names
// Global\HudTexturesGlobal.tpk stores, which is the same hash the engine's
// widget lookup uses -- MINIMAP_MASK hashing to 0x40CDC515 and
// MINIMAP_NORTH_INDICATOR to 0xF2A54430 is what cross-checks this against the
// two widget hashes Minimap's constructor (Speed.elf 0x80115cbc) holds.
#define MM_HASH_MASK   0x40CDC515u // MINIMAP_MASK,             256x256 DXT3
#define MM_HASH_BORDER 0x92A75587u // MINIMAP_BORDER,           128x128 DXT3
#define MM_HASH_NORTH  0xF2A54430u // MINIMAP_NORTH_INDICATOR,   32x16  DXT3
#define MM_HASH_CAR    MINIMAP_ICON_CAR // 16x16 DXT3, the player and AI art

// MINIMAP_ICON_SAFE_HOUSE, 16x16 DXT3: the art HUD_SingleRace.fng gives all
// six shop element types. See minimap.h for the whole element-to-art table.
#define MM_HASH_SHOP   MINIMAP_ICON_SAFE_HOUSE

// The career-event marker art; see minimap.h for which of the two an event
// gets and why that choice is the engine's rather than this port's.
#define MM_HASH_EVENT  MINIMAP_ICON_CIRCUIT
#define MM_HASH_SPON   MINIMAP_ICON_CROSS
// The showcase / world challenge marker (element 5).
#define MM_HASH_STAR   MINIMAP_ICON_STAR

// The GPS icon group's ring art and GPS_Search_Group's satellite (minimap.h,
// "The GPS"). The satellite is the one texture here InGameCommon.bun does not
// carry: InGameFreeRoam.bun's Global\HudTexturesSingleRace has it.
#define MM_HASH_GPS    0xE2848973u // MINIMAP_GPS_SELECTOR
#define MM_HASH_SAT    0xEED72BCEu // ICON_SATELLITE

typedef enum {
	MM_MASK, MM_BORDER, MM_NORTH, MM_CAR, MM_SHOP, MM_EVENT, MM_SPON, MM_GPS, MM_STAR,
	// From InGameFreeRoam.bun; everything before it is InGameCommon.bun's.
	MM_SAT,
	MM_COUNT
} MinimapTex;

static const u32 kTexHash[MM_COUNT] = {
	MM_HASH_MASK, MM_HASH_BORDER, MM_HASH_NORTH, MM_HASH_CAR, MM_HASH_SHOP,
	MM_HASH_EVENT, MM_HASH_SPON, MM_HASH_GPS, MM_HASH_STAR, MM_HASH_SAT,
};

// HUD_SingleRace.fng's authored sizes for the two icons (guid 902038 and the
// Minimap_Icon_Car pool slot), in canvas units. The canvas is 640x480 against
// a 320x240 panel, so a canvas unit is half a bottom-screen pixel -- but these
// are already small enough that halving them would put the compass under 8 px,
// so they are drawn at their authored size in pixels. The frame is the one
// piece that has to shrink: its 128-unit quad would otherwise swallow a third
// of the panel.
#define MM_NORTH_W 32.0f
#define MM_NORTH_H 16.0f
#define MM_CAR_W   12.0f
#define MM_CAR_H   16.0f
// C2D_Color32's packing as a constant expression, for the table below.
#define MM_RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))

// Each element type's widget colour, HUD_SingleRace.fng's own (see the table
// in minimap.h). A type this port never places is white, i.e. untinted.
static const u32 kElementColor[MINIMAP_ELEM_COUNT] = {
	[MINIMAP_ELEM_NONE]            = 0xFFFFFFFFu,
	[2]                            = 0xFFFFFFFFu,
	[MINIMAP_ELEM_STARTLINE]       = MM_RGBA(192, 192, 192, 255),
	[MINIMAP_ELEM_AIRACER]         = MM_RGBA(207, 128,  48, 255),
	[MINIMAP_ELEM_PLAYERCAR]       = MM_RGBA( 48, 207,  48, 255),
	// The widget's own 190,180,45 is INIT's; SyncBlink's "Static" (every
	// showcase zone's +0x14 is 0) sets this (minimap.h, the blink).
	[MINIMAP_ELEM_SHOWCASE]        = MM_RGBA(210, 195,  25, 255),
	[MINIMAP_ELEM_CRIB]            = MM_RGBA( 91,   2, 125, 255),
	[MINIMAP_ELEM_PAINTSHOP]       = MM_RGBA(114,  16,  29, 255),
	[MINIMAP_ELEM_PARTSSHOP]       = MM_RGBA( 16, 112,  16, 255),
	[MINIMAP_ELEM_ICESHOP]         = MM_RGBA(128, 128,   0, 255),
	[MINIMAP_ELEM_PERFORMANCESHOP] = MM_RGBA( 43, 100, 164, 255),
	[MINIMAP_ELEM_CARLOT]          = MM_RGBA( 82, 163, 163, 255),
	[MINIMAP_ELEM_CIRCUITEVENT]    = MM_RGBA(165,  60, 233, 255),
	[MINIMAP_ELEM_DRAGEVENT]       = MM_RGBA( 50, 130, 200, 255),
	[MINIMAP_ELEM_STREETEVENT]     = MM_RGBA(130, 190, 230, 255),
	[MINIMAP_ELEM_DRIFTEVENT]      = MM_RGBA(202, 244, 138, 255),
	[MINIMAP_ELEM_SPRINTEVENT]     = MM_RGBA( 14, 156,  14, 255),
	[MINIMAP_ELEM_URLEVENT]        = MM_RGBA(190, 180,  45, 255),
	[MINIMAP_ELEM_SUVEVENT]        = MM_RGBA(207, 128,  48, 255),
};

// The indicator widgets' own authored size -- every *INDICATOR_n in
// HUD_SingleRace.fng is 14.5 canvas units square. Drawn at that many pixels,
// the same way the car icon above is, so the two keep their authored
// proportion to each other on a panel that is not retail's 128-unit inset.
#define MM_MARKER_W 14.5f
#define MM_MARKER_H 14.5f
// MINIMAP_BORDER's quarter of the ring. HUD_SingleRace.fng places its four
// copies (900976-900979, all resource 14, all 128x128 at alpha 100) at exactly
// (+-16, +-16) from the map widget's own centre, so the arc's centre sits 16 of
// 128 inside its tile on each axis -- pixel (80, 80). Measured about that
// point, the shipped art sweeps 91..179 degrees and runs from radius 64 (where
// the mask's disc ends) out to 80, peaking at 136 over 68..72. So the quarter
// is exact, the ring begins precisely at the disc's rim rather than over the
// map, and its outer edge is 1.25 radii out. Fitting the art's own tile centre
// instead gives 74.8 and a 86..184 sweep, which is the same arc read off the
// wrong origin: four copies then overlap by eight degrees and composite into a
// bright tab at each compass point, which is what the first attempt showed.
// How solid the ring is drawn. The art's own peak is 136 of 255 and guid
// 900976-900979 carry 100 on top of that, both of them translucencies for
// compositing over the 3D view, and that is the whole difficulty. The art is a
// flat 8-texel white core at alpha 136 with a 7-texel black shade outside it;
// at the widget's 100 the core only lifts whatever is behind it, so on PC the
// slab itself never reads and what the eye takes for the rim is the pair of
// edges, a thin bright line over a soft dark one. This panel has only black
// behind it, where the same alpha reads as a dark haze and an opaque core
// reads as a white wall -- neither is the rim in the screenshot. The decode
// rescales the peak to opaque (see the load, and texpack.h for the mask's same
// case) and this takes it back to about the weight that line carries on PC.
// A port number, for a panel retail never drew.
#define MM_BORDER_ALPHA 112

#define MM_BORDER_ARC_C 0.125f  // 16 / 128, tile centre -> arc centre
#define MM_BORDER_ARC_R 0.625f  // 80 / 128, arc centre -> the ring's outer edge

// The north indicator (guid 902038, Minimap_North_indicator.tga, 32x16) is
// authored at canvas (-223, 70) against a map centre of (-223, 142): straight
// up, 72 of the widget's 128 from the centre, which is 1.125 radii -- on the
// ring, between its bright core and its outer edge. That authored point is
// where north is when the map is not turned; both minimaps run on Rotate
// (MinimapWrapper, minimap.h), so the widget travels round the ring with north
// as the map turns, which is what makes it a direction indicator rather than
// an ornament at the top.
#define MM_NORTH_R 0.5625f // 72 / 128 of the widget, centre -> the indicator

// Its authored colour. The .fng stores a widget's colour bytes the other way
// round from the order they are written here -- the paint shop's indicator is
// (29, 16, 114) on disc and (114, 16, 29) in kElementColor above, and every
// other element colour agrees -- so 902038's (48, 128, 207, 200) is this.
#define MM_NORTH_COLOR MM_RGBA(207, 128, 48, 200)

// GetScreenPositionForWorldCoordinate's limits, in windows (minimap.h).
// The pin circle, in windows (minimap.h, "The GPS"). One window is the widget
// across, so retail's 0.55 is 1.1 radii, which centres a pinned icon on the
// ring's bright core. Retail can do that because the core is a 20% wash there;
// the port draws it as a line, so an icon on it reads as a bead threaded on
// the rim. 0.50 is the disc's own edge, half the icon on the map, which is
// where the PC screenshot keeps them. A port number, and for the same reason
// as MM_BORDER_ALPHA. The rectangular panel's old deviation -- pinning to its
// own edges -- is retired with the shape either way.
#define MM_PIN 0.50f

#define MM_LIMIT_IDLE 1.0f  // 0x80327B0C
#define MM_LIMIT_GPS  10.0f // 0x80327B1C

// The GPS rings' script, 0x03826A28 (minimap.h): 1600 ms, looping, linear.
// Sizes are canvas units drawn as pixels, as the markers' are, under the
// group's 0.8.
#define MM_GPS_LOOP_MS 1600u
#define MM_GPS_SCALE   0.8f
#define MM_GPS_RING_DY 1.0f
// Minimap_GPS_Selector's two rings, guid 937103/937104, are authored
// (255, 128, 0, 255) on disc, which by the byte order above (see MM_NORTH_COLOR)
// is this. #286 took the bytes as written and drew the rings orange; every
// other colour in this file is reversed, and the ring is the AI racer's orange
// nowhere in the data.
#define MM_GPS_COLOR   MM_RGBA(0, 128, 255, 0)
// PORT (#471): the profile's GPS colour in its place, minimap_set_gps_colour().
static u32 s_gpsColor = MM_GPS_COLOR;

void minimap_set_gps_colour(u32 rgb) {
	s_gpsColor = MM_RGBA((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu, 0);
}
static const float kRingAMs[5]    = { 0.0f, 200.0f, 600.0f, 1200.0f, 1600.0f };
static const float kRingAAlpha[5] = { 0.0f, 255.0f, 180.0f, 0.0f, 0.0f };
static const float kRingASize[5]  = { 28.75f, 37.833f, 56.0f, 70.0f, 70.0f };
static const float kRingBMs[4]    = { 0.0f, 400.0f, 1000.0f, 1600.0f };
static const float kRingBAlpha[4] = { 0.0f, 255.0f, 180.0f, 0.0f };
static const float kRingBSize[4]  = { 28.75f, 28.75f, 56.0f, 70.0f };

struct Minimap {
	TrackMap map;
	C3D_Tex tex[MM_COUNT];
	Tex3DS_SubTexture sub[MM_COUNT];
	C2D_Image img[MM_COUNT];
	bool texValid[MM_COUNT];

	// The art window, rebuilt every frame from the car's position. Separate
	// from map.subtex, which stays the whole-texture one trackmap_load() wrote.
	Tex3DS_SubTexture window;
	C2D_Image windowImg;

	float originX, originY, span, zoom;
};

// main.c's scissorLogical(), which is static there. Same contract: logical
// screen coordinates with the origin top-left, converted to the GPU's rotated
// framebuffer axes, and C2D_Flush() first because the scissor applies to
// everything already queued in the batch.
#define MM_SCREEN_W 320.0f
#define MM_SCREEN_H 240.0f

static void mmScissor(float x, float y, float w, float h) {
	float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	if (x0 < 0.0f) x0 = 0.0f;
	if (y0 < 0.0f) y0 = 0.0f;
	if (x1 > MM_SCREEN_W) x1 = MM_SCREEN_W;
	if (y1 > MM_SCREEN_H) y1 = MM_SCREEN_H;
	if (x1 <= x0 || y1 <= y0) {
		x1 = x0 + 1.0f;
		y1 = y0 + 1.0f;
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL,
		(u32)(MM_SCREEN_H - y1), (u32)(MM_SCREEN_W - x1),
		(u32)(MM_SCREEN_H - y0), (u32)(MM_SCREEN_W - x0));
}

static void mmScissorReset(void) {
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// A linear key track at `ms` (the engine's linear interpolation), held at
// its ends.
static float mmKeys(const float* t, const float* v, int n, float ms) {
	if (ms <= t[0])
		return v[0];
	for (int i = 1; i < n; i++) {
		if (ms <= t[i]) {
			const float f = (ms - t[i - 1]) / (t[i] - t[i - 1]);
			return v[i - 1] + (v[i] - v[i - 1]) * f;
		}
	}
	return v[n - 1];
}

// The panel's GetScreenPositionForWorldCoordinate. (u, v) is the icon's
// offset from the car in pixels, already turned with the map; a window is
// `windowPx` across. False past `limit` windows. Otherwise an icon off the
// panel is pulled in along the same line until it sits `pad` inside the
// edge -- this rectangle's stand-in for retail's 0.55 circle.
static bool mmPlace(float u, float v, float windowPx, float limit, float radius,
		float pad, float* du, float* dv) {
	const float r = sqrtf(u * u + v * v);
	if (r >= limit * windowPx)
		return false;
	// Retail pins an icon past the round inset back onto it along the same
	// line (minimap.h, "The GPS"). The panel is that round inset again
	// (TASKS.md #287), so the rim is a radius and not a pair of edges. `pad`
	// is what to keep clear of it; retail keeps nothing clear, because its pin
	// circle already lies outside the disc.
	const float rim = radius - pad > 1.0f ? radius - pad : 1.0f;
	if (r > rim && r > 0.0f) {
		u *= rim / r;
		v *= rim / r;
	}
	*du = u;
	*dv = v;
	return true;
}

Minimap* minimap_open(const char* mapPath, int trackId, const char* hudBunPath,
	const char* freeRoamBunPath, float originX, float originY, float span, float zoom,
	void (*log)(const char*))
{
	Minimap* m = (Minimap*)calloc(1, sizeof(Minimap));
	if (!m)
		return NULL;
	trackmap_init(&m->map);
	m->originX = originX;
	m->originY = originY;
	m->span = span;
	// A zero zoom would divide by zero in Tick's `0.3f / zoom`. No shipped
	// record carries one -- the three values in the PC table are 1, 1.5 and 2 --
	// so this is a guard, not a stand-in for an unread field.
	m->zoom = (zoom > 0.0f) ? zoom : 1.0f;

	char b[700];
	// Native size, not TRACKMAP_MAX_DIM: a 0.2-wide window of the art is
	// blown up to fill the panel, so halved source texels would show.
	if (trackmap_load(&m->map, mapPath, trackId, TRACKMAP_FULL_DIM)) {
		if (log) {
			snprintf(b, sizeof(b), "minimap: TRACKMAP%d loaded (%dx%d), art origin "
				"%.2f,%.2f span %.2f zoom %.1f", trackId,
				m->map.subtex.width, m->map.subtex.height,
				(double)originX, (double)originY, (double)span, (double)m->zoom);
			log(b);
		}
	} else if (log) {
		snprintf(b, sizeof(b), "minimap: %s MISSING -- frame only, no map art", mapPath);
		log(b);
	}

	// Global\HudTexturesGlobal.tpk is the second of InGameCommon.bun's two
	// containers; texpack_load() scans the whole file, so no offset is needed.
	// The file is 1.5 MB and is held only for the three decodes below.
	TexPack pack;
	if (texpack_load_shared(hudBunPath, &pack)) {
		for (int i = 0; i < MM_SAT; i++) {
			const TexPackEntry* e = texpack_find(&pack, kTexHash[i]);
			// maxDim 0: these are 128x128 and smaller already, and the compass
			// is 32x16 -- halving any of them would be visible.
			// The mask is a shape and the frame a rim, not translucencies (see
			// texpack.h, and the frame's own note in minimap_draw): both are
			// authored to sit over the 3D view, and this panel has only black
			// behind it, which left the ring a middling grey instead of the
			// light one retail draws.
			const bool got = e && (i == MM_MASK || i == MM_BORDER
				? texpack_decode_entry_alpha_full(e, &m->tex[i], &m->sub[i])
				: texpack_decode_entry(e, 0, false, &m->tex[i], &m->sub[i]));
			if (got) {
				m->texValid[i] = true;
				m->img[i].tex = &m->tex[i];
				m->img[i].subtex = &m->sub[i];
			} else if (log) {
				snprintf(b, sizeof(b), "minimap: texture 0x%08lx not in %s",
					(unsigned long)kTexHash[i], hudBunPath);
				log(b);
			}
		}
		texpack_release(&pack);
	} else if (log) {
		snprintf(b, sizeof(b), "minimap: %s would not load (texpack stage %d) -- "
			"no frame, no compass", hudBunPath, g_texpackFail.stage);
		log(b);
	}
	if (freeRoamBunPath && texpack_load_shared(freeRoamBunPath, &pack)) {
		for (int i = MM_SAT; i < MM_COUNT; i++) {
			const TexPackEntry* e = texpack_find(&pack, kTexHash[i]);
			if (e && texpack_decode_entry(e, 0, false, &m->tex[i], &m->sub[i])) {
				m->texValid[i] = true;
				m->img[i].tex = &m->tex[i];
				m->img[i].subtex = &m->sub[i];
			} else if (log) {
				snprintf(b, sizeof(b), "minimap: texture 0x%08lx not in %s",
					(unsigned long)kTexHash[i], freeRoamBunPath);
				log(b);
			}
		}
		texpack_release(&pack);
	} else if (freeRoamBunPath && log) {
		snprintf(b, sizeof(b), "minimap: %s would not load -- no GPS satellite",
			freeRoamBunPath);
		log(b);
	}

	m->windowImg.tex = &m->map.tex;
	m->windowImg.subtex = &m->window;
	return m;
}

void minimap_close(Minimap* m) {
	if (!m)
		return;
	trackmap_free(&m->map);
	for (int i = 0; i < MM_COUNT; i++) {
		if (m->texValid[i])
			C3D_TexDelete(&m->tex[i]);
	}
	free(m);
}

// Re-textures the one map widget, which is the whole of what the engine does
// when the map has to show a route (see minimap.h). `mapPath` is the file for
// `trackId`; the placement comes from that track's own record, because the
// caller already has the TrackDb open and this file does not keep one.
bool minimap_set_track(Minimap* m, const char* mapPath, int trackId,
		float originX, float originY, float span, float zoom,
		void (*log)(const char*)) {
	char b[700];
	if (!m || !mapPath)
		return false;
	if (m->map.valid && m->map.trackId == trackId)
		return true;
	// The art the panel is pointing at is freed by trackmap_load() before the
	// new one is read, so a failed read leaves no map rather than the old one.
	// The window image keeps pointing at &m->map.tex either way, which is a
	// fixed address inside the struct.
	if (!trackmap_load(&m->map, mapPath, trackId, TRACKMAP_FULL_DIM)) {
		if (log) {
			snprintf(b, sizeof(b), "minimap: %s MISSING -- no map art for track %d",
				mapPath, trackId);
			log(b);
		}
		return false;
	}
	m->originX = originX;
	m->originY = originY;
	m->span = span;
	m->zoom = (zoom > 0.0f) ? zoom : 1.0f;
	if (log) {
		snprintf(b, sizeof(b), "minimap: TRACKMAP%d loaded (%dx%d), art origin "
			"%.2f,%.2f span %.2f zoom %.1f", trackId,
			m->map.subtex.width, m->map.subtex.height,
			(double)originX, (double)originY, (double)span, (double)m->zoom);
		log(b);
	}
	return true;
}

bool minimap_have_map(const Minimap* m) {
	return m && m->map.valid;
}

bool minimap_map_art(const Minimap* m, C2D_Image* out, float* originX, float* originY,
		float* span) {
	if (!m)
		return false;
	// The placement holds without the art: the markers still go where they
	// belong over an empty panel.
	*originX = m->originX;
	*originY = m->originY;
	*span = m->span;
	if (!m->map.valid) {
		memset(out, 0, sizeof(*out));
		return false;
	}
	*out = m->map.image;
	return true;
}

bool minimap_satellite(const Minimap* m, C2D_Image* out) {
	if (!m || !m->texValid[MM_SAT])
		return false;
	*out = m->img[MM_SAT];
	return true;
}

void minimap_draw(Minimap* m, float worldX, float worldY, float headingRad,
	float x, float y, float w, float h, MinimapMode mode,
	const MinimapMarker* markers, int markerCount, const MinimapGps* gps)
{
	if (!m || mode == MINIMAP_MODE_OFF || w <= 0.0f || h <= 0.0f)
		return;

	// Tick's own rotation decision: rotate only on option value 2.
	//
	// The angle is derived rather than transcribed. Tick does
	// FEngSetRotationZ(mapImage, -bATan(dir.y, dir.x) in degrees), but neither
	// bATan's zero direction nor FEngSetRotationZ's sign is established
	// anywhere this port can read, and the pair is only self-consistent up to a
	// quarter turn (Tick gives the compass -angle + 270, i.e. the map's angle
	// less 90, which cannot both be right under one convention). So the
	// relationship is worked out from the art instead, which is unambiguous:
	//
	//   The unrotated art has north up: artY = (originY - worldY) / span, so
	//   the V axis runs south. Screen +x is world +X, screen +y is world -Y.
	//   The car's forward in that frame is (cos yaw, -sin yaw). citro2d rotates
	//   content by (x cos - y sin, x sin + y cos), which on a y-down screen is
	//   clockwise for a positive angle. Solving for the angle that sends the
	//   car's forward to screen up (0, -1) gives yaw - pi/2.
	const float full = headingRad - (float)M_PI * 0.5f;
	const float mapAngle = (mode == MINIMAP_MODE_ROTATE) ? full : 0.0f;
	// The car icon's art points up. In rotate mode the map has already turned
	// the car's heading to the top of the screen, so the icon stays put; in
	// static mode it carries the whole turn itself.
	const float carAngle = mapAngle - full;

	// PORT (TASKS.md #287), the user 2026-10-27: "making the minimap on the
	// bottom screen in-game a big circle instead like what you see on PC
	// (PC's is a smaller circle but still, this just looks ugly)". Retail's
	// minimap is a circle and always was -- MINIMAP_MASK is a filled disc in
	// alpha and MINIMAP_BORDER a quarter-arc centred on its own tile, four of
	// which ring it -- and the full-width rectangle was this port's deviation
	// (minimap.h, "What this port does differently"). The panel goes back to
	// retail's shape, at the biggest size the touch screen holds: the circle
	// inscribed in the rect the caller gives, centred in it. Every measurement
	// below is then retail's own again over a square widget -- the aperture is
	// one window across the diameter, not across a short axis.
	const float panelX = x, panelY = y, panelW = w, panelH = h;
	const float panel = (w < h) ? w : h;
	// The widget is sized by its frame, not by its disc: MINIMAP_BORDER's soft
	// outer skirt reaches MM_BORDER_ARC_R of the widget from its centre, past
	// the mask's own half, so sizing the disc to the panel would push the ring
	// off the screen's bottom edge. The skirt touches the panel instead and
	// the disc is what is left, which is still a circle the height of the
	// touch screen below the bank line.
	// Snapped to a whole multiple of eight. Whole pixels because mmScissor
	// truncates, so a square that ends on a fraction leaves the scissor a pixel
	// wider than the mask quad and a column of map art survives unmasked down
	// the square's right and bottom edges; a multiple of eight because the
	// frame's four quarters are offset by an eighth of it (MM_BORDER_ARC_C) and
	// each quarter covers its own quadrant exactly, so a half-pixel offset puts
	// a visible step in the ring where two of them meet.
	const float d = floorf(panel * 0.5f / MM_BORDER_ARC_R / 8.0f) * 8.0f;
	x = floorf(panelX + (panelW - d) * 0.5f);
	y = floorf(panelY + (panelH - d) * 0.5f);
	w = d;
	h = d;

	const float cx = x + w * 0.5f;
	const float cy = y + h * 0.5f;

	// The map and its mask are bounded by the widget square -- the rotated
	// quad is bigger than it, and the mask only covers the square, so nothing
	// outside may survive. Everything after the mask is bounded by the panel,
	// which is what gives the frame's skirt its room.
	mmScissor(x, y, w, h);

	// Tick's art mapping, literally.
	const float artX = (worldX - m->originX) / m->span;
	const float artY = (m->originY - worldY) / m->span;

	// Retail's aperture is `0.3f / zoom` of the art across a 128-unit
	// square widget. Keeping that scale means the panel's SHORT axis shows
	// the same stretch of city; the long axis then shows more of it. The
	// quad drawn is the square that covers the panel at any rotation, so
	// the scissor -- not an empty corner -- is what bounds the picture.
	//
	// The markers below need the same aperture, and it does not depend on the
	// map art having loaded -- only on the track record's placement fields --
	// so it is worked out here rather than inside the map's own block.
	const float window = 0.3f / m->zoom;
	const float side = sqrtf(w * w + h * h);
	const float shortAxis = (w < h) ? w : h;
	const float half = (side * 0.5f / shortAxis) * window;

	if (m->map.valid) {
		// trackmap_load() left subtex describing the whole image inside a
		// padded power-of-two texture: left 0, top 1, right srcW/texW,
		// bottom 1 - srcH/texH. Re-derive those two fractions rather than
		// assume the map was not padded or downscaled.
		const float fx = m->map.subtex.right - m->map.subtex.left;
		const float fy = m->map.subtex.top - m->map.subtex.bottom;

		m->window.width = (u16)(side + 0.5f);
		m->window.height = (u16)(side + 0.5f);
		m->window.left = m->map.subtex.left + (artX - half) * fx;
		m->window.right = m->map.subtex.left + (artX + half) * fx;
		m->window.top = m->map.subtex.top - (artY - half) * fy;
		m->window.bottom = m->map.subtex.top - (artY + half) * fy;

		C2D_DrawImageAtRotated(m->windowImg, cx, cy, 0.1f, mapAngle, NULL, 1.0f, 1.0f);

		// psMiniMapAlphaMask (Speed.elf 0x801e6640) multiplies MINIMAP_MASK's
		// alpha into what the map widget drew, and that is what rounds the
		// inset. Same thing here, as a blend: dst *= src.alpha clears the
		// square's corners and leaves the disc. The mask's own 204 is a
		// translucency for compositing over the 3D view and is rescaled away
		// at load (texpack.h), so inside the disc the art keeps its full
		// brightness. citro2d only issues a quad on a flush and reads the
		// blend then, so both sides are flushed.
		if (m->texValid[MM_MASK]) {
			C2D_Flush();
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_SRC_ALPHA,
				GPU_ZERO, GPU_SRC_ALPHA);
			C2D_DrawImageAt(m->img[MM_MASK], x, y, 0.15f, NULL,
				w / (float)m->sub[MM_MASK].width, h / (float)m->sub[MM_MASK].height);
			C2D_Flush();
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
				GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		}
	}

	mmScissor(panelX, panelY, panelW, panelH);

	// The frame, before every icon. HUD_SingleRace.fng authors it at z 100, the
	// farthest of the screen's widgets -- the mask is at 10, an element
	// indicator at 8, the north indicator and the GPS rings at 1 -- so retail's
	// ring is under the map and under every icon, and a marker the placement
	// pins onto it is drawn over it rather than swallowed by it. Under the
	// icons is kept; under the map is not, and cannot be: the mask pass
	// multiplies the square's corners to nothing, and the ring runs through
	// those corners on its way round, so drawing it first costs it everything
	// between the compass points. It makes no difference to the picture,
	// because the ring begins exactly where the mask's disc ends and the two
	// never overlap.
	//
	// HUD_SingleRace.fng's four Minimap_Border copies (guids
	// 900976-900979) at 0 / 90 / 180 / 270 degrees. The art is one quarter of
	// the ring, and it is drawn about its own tile's centre -- over the
	// 128x128 tile the opaque band runs from radius 41 to radius 64, so the
	// ring's outer edge is the tile inscribed. Four copies therefore sit at
	// ONE point, the circle's centre, each turned a quarter turn; drawing them
	// at four corners (which is what a rectangular panel asked for) is what
	// made the brackets the user saw. Scaled by the tile, the ring lands
	// exactly on the mask's rim, which is inscribed in the same square.
	if (m->texValid[MM_BORDER]) {
		const float bs = w / (float)m->sub[MM_BORDER].width;
		const float bsy = h / (float)m->sub[MM_BORDER].height;
		C2D_ImageTint rim;
		C2D_PlainImageTint(&rim, MM_RGBA(255, 255, 255, MM_BORDER_ALPHA), 1.0f);
		C2D_SetTintMode(C2D_TintMult);
		// Half a pixel short of the arc's centre, on both axes. A quarter's art
		// stops at texel 79 and its centre is texel 80, so two quarters abut
		// exactly at the circle's centre line -- but the art is drawn larger
		// than 1:1 here, and the pixel beside that line samples a kernel that
		// reaches texel 80, which is transparent. Both sides lose a fifth of
		// their alpha and the ring shows a hairline at each compass point. The
		// nudge puts texel 79's own centre under that pixel's centre instead,
		// so the kernel stays inside the art: 16*bs - (0.5*bs - 0.5), which is
		// an eighth of a pixel at this size and leaves the four arcs concentric
		// to well under a pixel.
		const float ox = MM_BORDER_ARC_C * w - (0.5f * bs - 0.5f);
		const float oy = MM_BORDER_ARC_C * h - (0.5f * bsy - 0.5f);
		for (int k = 0; k < 4; k++) {
			// C2D turns an image about its own centre, and the arc's centre is
			// not that -- it is MM_BORDER_ARC_C further along both axes. The
			// tile is placed so the ARC's centre lands on the circle's, which
			// is what HUD_SingleRace.fng does with its own +-16 (above) and is
			// the whole of it: four tiles sharing a TILE centre overlap and
			// composite to the four bright blocks #287 was raised about.
			const float a = (float)k * (float)M_PI * 0.5f;
			const float c = cosf(a), sn = sinf(a);
			C2D_DrawImageAtRotated(m->img[MM_BORDER],
				cx - (ox * c - oy * sn), cy - (ox * sn + oy * c), 0.05f, a, &rim,
				bs, bsy);
		}
		C2D_SetTintMode(C2D_TintSolid);
	}

	// The static markers. AddStaticElement is handed a world point and the
	// panel shows `2 * half` of art across `side` pixels, so a marker's offset
	// from the car is its art-space offset at that scale, turned by the map's
	// own rotation. Drawn above the map and below the frame, the compass and
	// the car, which is the order their depths already put them in.
	//
	// The panel's short axis is one window (see above), and the limit is
	// retail's: 10 windows while the GPS is engaged or the stage is 0,
	// otherwise 1.
	const float perArt = (m->span > 0.0f && half > 0.0f) ? side / (2.0f * half) : 0.0f;
	const float ca = cosf(mapAngle);
	const float sa = sinf(mapAngle);
	const float limit = (gps && (gps->engaged || gps->stageZero)) ? MM_LIMIT_GPS : MM_LIMIT_IDLE;
	if (markers && markerCount > 0 && perArt > 0.0f) {
		// The widget colour multiplies the texels, as the engine's vertex
		// colour does -- C2D_TintSolid would replace them instead.
		C2D_SetTintMode(C2D_TintMult);
		for (int i = 0; i < markerCount; i++) {
			int slot = -1;
			for (int k = 0; k < MM_COUNT; k++) {
				if (kTexHash[k] == markers[i].texHash) {
					slot = k;
					break;
				}
			}
			if (slot < 0 || !m->texValid[slot])
				continue;
			// artX/artY cancel: the difference of the two art mappings is just
			// the world delta over the span, with Y flipped as the art's V
			// axis runs south.
			const float u = ((markers[i].worldX - worldX) / m->span) * perArt;
			const float v = ((worldY - markers[i].worldY) / m->span) * perArt;
			// citro2d rotates content by (x cos - y sin, x sin + y cos), the
			// same transform the map quad above gets.
			float du, dv;
			if (!mmPlace(u * ca - v * sa, u * sa + v * ca, shortAxis, limit,
					MM_PIN * shortAxis, 0.0f, &du, &dv))
				continue;
			const float sx = cx + du;
			const float sy = cy + dv;
			const u8 el = markers[i].element;
			C2D_ImageTint tint;
			u32 col = kElementColor[el < MINIMAP_ELEM_COUNT ? el : MINIMAP_ELEM_NONE];
			if (markers[i].dim)
				col = (col & 0x00FFFFFFu) | 0x80000000u;
			C2D_PlainImageTint(&tint, col, 1.0f);
			if (markers[i].turns) {
				// The same transform the player's own icon gets below, with
				// this marker's heading in place of the camera's.
				C2D_DrawImageAtRotated(m->img[slot], sx, sy, 0.2f,
					mapAngle - (markers[i].heading - (float)M_PI * 0.5f), &tint,
					MM_MARKER_W / (float)m->sub[slot].width,
					MM_MARKER_H / (float)m->sub[slot].height);
			} else {
				// Upright: a shop or an event start does not face anywhere.
				C2D_DrawImageAt(m->img[slot], sx - MM_MARKER_W * 0.5f,
					sy - MM_MARKER_H * 0.5f, 0.2f, &tint,
					MM_MARKER_W / (float)m->sub[slot].width,
					MM_MARKER_H / (float)m->sub[slot].height);
			}
		}
		C2D_SetTintMode(C2D_TintSolid);
	}

	// The GPS icon group: the two rings at GetMinimapArtCenter's point,
	// through the same placement, above the markers.
	if (gps && gps->icon && perArt > 0.0f && m->texValid[MM_GPS]) {
		const float u = ((gps->iconX - worldX) / m->span) * perArt;
		const float v = ((worldY - gps->iconY) / m->span) * perArt;
		float du, dv;
		if (mmPlace(u * ca - v * sa, u * sa + v * ca, shortAxis, limit,
				MM_PIN * shortAxis, 0.0f, &du, &dv)) {
			const float ms = (float)(gps->ms % MM_GPS_LOOP_MS);
			const float alpha[2] = { mmKeys(kRingAMs, kRingAAlpha, 5, ms),
			                         mmKeys(kRingBMs, kRingBAlpha, 4, ms) };
			const float size[2] = { mmKeys(kRingAMs, kRingASize, 5, ms),
			                        mmKeys(kRingBMs, kRingBSize, 4, ms) };
			const float rw = (float)m->sub[MM_GPS].width, rh = (float)m->sub[MM_GPS].height;
			C2D_SetTintMode(C2D_TintMult);
			for (int k = 0; k < 2; k++) {
				const float px = size[k] * MM_GPS_SCALE;
				C2D_ImageTint tint;
				C2D_PlainImageTint(&tint, s_gpsColor | ((u32)(alpha[k] + 0.5f) << 24), 1.0f);
				C2D_DrawImageAt(m->img[MM_GPS], cx + du - px * 0.5f,
					cy + dv + MM_GPS_RING_DY * MM_GPS_SCALE - px * 0.5f, 0.25f, &tint,
					px / rw, px / rh);
			}
			C2D_SetTintMode(C2D_TintSolid);
		}
	}


	// The north indicator rides the ring at north. The map turns under it, so
	// north is screen-up only while the map is unrotated; the rest of the time
	// the indicator is that far round the ring, pointing out of it, which is
	// the direction the city's north lies in from the car. The map quad is
	// drawn through the same rotation, and citro2d turns content by
	// (x cos - y sin, x sin + y cos), so screen-up (0, -1) becomes
	// (sin, -cos) -- and the art, which points up, is turned to match.
	if (m->texValid[MM_NORTH]) {
		const float nr = MM_NORTH_R * w;
		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, MM_NORTH_COLOR, 1.0f);
		C2D_SetTintMode(C2D_TintMult);
		C2D_DrawImageAtRotated(m->img[MM_NORTH],
			cx + nr * sinf(mapAngle), cy - nr * cosf(mapAngle), 0.4f, mapAngle,
			&tint,
			MM_NORTH_W / (float)m->sub[MM_NORTH].width,
			MM_NORTH_H / (float)m->sub[MM_NORTH].height);
		C2D_SetTintMode(C2D_TintSolid);
	}

	// The player, at the centre of the window by construction -- which is why
	// Minimap::GetScreenPositionForWorldCoordinate is only ever asked about
	// other things.
	// Green, PLAYERCARINDICATOR_1's colour.
	if (m->texValid[MM_CAR]) {
		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, kElementColor[MINIMAP_ELEM_PLAYERCAR], 1.0f);
		C2D_SetTintMode(C2D_TintMult);
		C2D_DrawImageAtRotated(m->img[MM_CAR], cx, cy, 0.5f, carAngle, &tint,
			MM_CAR_W / (float)m->sub[MM_CAR].width,
			MM_CAR_H / (float)m->sub[MM_CAR].height);
		C2D_SetTintMode(C2D_TintSolid);
	}

	mmScissorReset();
}
