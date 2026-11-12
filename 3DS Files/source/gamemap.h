#pragma once
#include <3ds.h>
#include <stdbool.h>

// GameWorldMapScreen (UI_InGame_WorldMap.fng), free roam's world map --
// TASKS.md #286. This is the screen's model, kept apart from main.c's widgets
// the way cribmap.h is for the crib's map, so that what retail computes reads
// on its own:
//
//   * WorldMapWrapper (the ctor and Tick): one element per radar-visible
//     trigger zone, placed through the zoom region's window over the city art;
//   * SetupList: the legend's rows, one per element type, with the filter
//     each row shows and toggles (PlayerCareerState +0x7F5C, the save's
//     mapFilter);
//   * ScrollZoomRegion over SectionInfoTable's six regions, and the port's
//     free view on top of them (below);
//   * the GPS locator: BuildTriggerList's cursor list for the highlighted row,
//     ScrollGPS, RebuildGPSLocator, and UpdateGPSButtonText's disabled flag.
//
// main.c owns the widgets, the strings, the textures, the dialogs and the pad.

// WorldMapElementType: the legend's order, and Label[] in WorldMapWrapper's
// "%s%d" widget names. 13 (showcase) is never placed here: the showcase zones
// are not on the minimap either. 14 (AI racer) is not a zone element either:
// the ctor (0x800D671C) adds an AICarWMElement for every AI car in the World
// -- free roam's two encounter racers included -- and main.c's drawGameMap()
// draws them straight from the cars, as it does the player's.
typedef enum {
	GAMEMAP_WME_CIRCUIT = 0,
	GAMEMAP_WME_DRAG,
	GAMEMAP_WME_STREET,
	GAMEMAP_WME_DRIFT,
	GAMEMAP_WME_SPRINT,
	GAMEMAP_WME_URL,
	GAMEMAP_WME_SUV,
	GAMEMAP_WME_PAINTSHOP,
	GAMEMAP_WME_PARTSSHOP,
	GAMEMAP_WME_PERFORMANCESHOP,
	GAMEMAP_WME_CARLOT,
	GAMEMAP_WME_ICESHOP,
	GAMEMAP_WME_CRIB,
	GAMEMAP_WME_SHOWCASE,
	GAMEMAP_WME_AIRACER,
	GAMEMAP_WME_COUNT
} GameMapElemType;

// The closest the view zooms: a quarter of the art's side, a little past
// SectionInfoTable's smallest region (171 of 512).
#define GAMEMAP_ZOOM_MIN 0.25f

// SetupList's slots, and the rows the package authors (DATA_A_1..DATA_D_14).
#define GAMEMAP_SLOTS      16
#define GAMEMAP_ROW_WIDGETS 14
// SetupList gives DATA_D (the sponsor cross) to the first seven rows only.
#define GAMEMAP_CROSS_ROWS 7
#define GAMEMAP_MAX_ELEMS  256
#define GAMEMAP_SECTIONS   6
// The save's 16 filter bytes, one per trigger zone type.
#define GAMEMAP_FILTERS    16

typedef struct {
	float worldX, worldY;  // the zone box's midpoint
	u8 wme;                // GetTranslatedZoneType(zoneType)
	u8 zoneType;           // GetTriggerZoneType: the filter's index
	bool hasEvent;         // the zone carries an event record (SetupList's split)
	bool sponsor;          // event +0x34 == 1: drawn with Minimap_Icon_Cross
	int pool;              // the "%s%d" counter within its type, from 1
	const void* zone;      // main.c's zone, handed back for GPSManager::Start
	// Its place on the whole art (0..1, V down), and whether it is on it at
	// all: IsOnWorldMap at the full map.
	float au, av;
	bool onArt;
	// Tick's: the canvas centre, and whether it is inside the map's rect.
	float sx, sy;
	bool onMap;
} GameMapElem;

typedef struct {
	u8 wme;
	u8 zoneType;           // the type's first zone's: the legend text and filter
	bool sponsor;          // any zone of the type carries a sponsor event
} GameMapRow;

typedef struct {
	int elemCount;
	GameMapElem elems[GAMEMAP_MAX_ELEMS];
	int rowCount;
	GameMapRow rows[GAMEMAP_SLOTS];
	int highlight;         // the legend's current row
	unsigned char* filter; // the profile's mapFilter[16], 1 = shown

	// ScrollZoomRegion: the region shown and the stage's last one.
	int section, maxSection;
	// PORT: the view (the user's, 2026-10-27): L and R zoom out and in and the
	// Circle Pad pans, where retail only steps through SectionInfoTable. A
	// square window over the art, `zoom` of its side (1 is the whole art, the
	// regions are 0.33..0.6 of it), centred on (viewU, viewV). `section` is
	// then the region the view is over, for ZOOM_TEXT, and the arrows' step
	// still lands on retail's regions. While `sectionPinned` the view came
	// from ScrollZoomRegion and `section` is that step's own region, not
	// one derived back from the window: the regions overlap, so a smaller
	// one often holds a larger one's centre and the derivation named a
	// region the step never chose (the first arrow read 4 of 0..3).
	bool sectionPinned;
	float zoom, viewU, viewV;
	// Tick's window over the art (0..1, V down) and the icon size it implies.
	float u0, v0, u1, v1;
	float iconSize;

	// TrackInfo +0xAC / +0xB0 / +0xB4, and MAP_IMAGE's rect in canvas units.
	float originX, originY, span;
	float mapCx, mapCy, mapW, mapH;

	// BuildTriggerList's cursor, as element indices, and the locator.
	int cursorCount;
	short cursor[GAMEMAP_MAX_ELEMS];
	int cursorAt;
	bool locatorShown;
	bool rebuild;          // RebuildGPSLocator on the next tick
	bool gpsDisabled;      // UpdateGPSButtonText's flag: A does nothing
} GameMap;

// GetTranslatedZoneType (zone type -> element type), and the reverse
// ToggleFilter writes through. 0xFF for a type with no element.
u8 gamemap_wme_for_zone_type(int zoneType);

// The ctor's state before any zone is added: region 0 (the full map), the
// stage's last region, the profile's filters.
void gamemap_init(GameMap* m, unsigned char* filter, int careerStage,
	float originX, float originY, float span,
	float mapCx, float mapCy, float mapW, float mapH);

// WorldMapWrapper's per-zone step. `poolMax[wme]` is how many "%s%d" widgets
// the package authors for that type; a zone past them is not placed (retail's
// FindObject would come back NULL). Returns the element, or -1.
int gamemap_add(GameMap* m, float worldX, float worldY, int zoneType, bool hasEvent,
	bool sponsor, const void* zone, const int poolMax[GAMEMAP_WME_COUNT]);

// SetupList, then StartFiltering and StartGPSLocator, as the ctor runs them.
void gamemap_setup(GameMap* m);

// IsRenderable for a trigger element: its type's filter byte.
bool gamemap_renderable(const GameMap* m, const GameMapElem* e);
// Whether row `r` is shown on the map (Activate) or filtered off (Deactivate).
bool gamemap_row_on(const GameMap* m, int r);

// ScrollHighlight: -1 up, +1 down, no wrap. True when it moved (the sound).
bool gamemap_scroll_highlight(GameMap* m, int dir, bool gpsIdle);
// ToggleFilter on the highlighted row. False when there is no row.
bool gamemap_toggle_filter(GameMap* m, bool gpsIdle);
// ScrollZoomRegion: -1 prev (wraps to the stage's last), +1 next, from the
// region the view is over; the view then shows that region whole.
void gamemap_scroll_zoom(GameMap* m, int dir);
// The port's view: the window's side times `factor`, about its centre, and
// a pan of (du, dv) windows. Both keep the window on the art and return
// whether it moved.
bool gamemap_zoom_by(GameMap* m, float factor);
bool gamemap_pan(GameMap* m, float du, float dv);
// Pans the view to the cursor's element when it is outside it.
bool gamemap_show_cursor(GameMap* m);
// ScrollGPS: -1 prev, +1 next, both wrapping. True when the list has more
// than one element, which is when retail plays the sound.
bool gamemap_scroll_gps(GameMap* m, int dir);
// Jumps the cursor to element `e` if it is in the list (the port's tap).
bool gamemap_cursor_to(GameMap* m, int e);

// WorldMapWrapper::Tick, then the rebuild if one is pending.
void gamemap_tick(GameMap* m, bool gpsIdle);
// RebuildGPSLocator, now.
void gamemap_rebuild_locator(GameMap* m, bool gpsIdle);
// UpdateGPSButtonText's disabled flag.
void gamemap_update_button(GameMap* m, bool gpsIdle);

// Tick's placement for any world point, the player's included. False when
// it falls outside the map's rect.
bool gamemap_place(const GameMap* m, float worldX, float worldY, float* sx, float* sy);

// The element under the cursor, NULL on an empty list.
const GameMapElem* gamemap_cursor(const GameMap* m);
