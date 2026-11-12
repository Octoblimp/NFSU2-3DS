#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#include "trackmap.h"

// The free-roam minimap (TASKS.md #5), the bottom screen's own content while
// the car is in the world.
//
// Retail provenance
// -----------------
// Everything about *what* is drawn and *where it points* is the shipped game's,
// recovered from the GameCube build because Ghidra cannot decompile the PC
// ones -- Gekko paired-single opcodes make its PPC decompiler return
// halt_baddata() for every function in this class, which is why
// tools/gc_disasm.py exists:
//
//   Minimap::Minimap(const char*)                     Speed.elf 0x80115cbc
//   Minimap::Tick                                               0x80115fbc
//   Minimap::GetScreenPositionForWorldCoordinate                0x8011668c
//   GOExploringMiniMap::Act (the option toggle)                 0x800ec0e4
//   GameplaySettings::Default (the option's default)            0x80130b88
//
// The constructor binds three widgets by object hash, and those hashes are
// exactly the bStringHash of the three texture names this file loads:
//
//   +0x08  0x90FE80F3  the map image   (re-textured per track, see trackmap.h)
//   +0x0C  0x40CDC515  MINIMAP_MASK
//   +0x10  0xF2A54430  MINIMAP_NORTH_INDICATOR
//
// and +0x14 holds GetTrackInfo(currentTrackId), whose +0xAC/+0xB0/+0xB4/+0xC0
// are the four map-art fields trackdb.h documents.
//
// Tick's placement maths, transcribed:
//
//   artX  = (worldX - originX) / span
//   artY  = (originY - worldY) / span        // +0xB0 is the art's TOP edge
//   half  = (0.3f / zoom) * 0.5f             // 0.1 at Bayview's zoom 1.5
//   SetTrackValue(mapImage, 5, {artX - half, artY - half})   // UV upper-left
//   SetTrackValue(mapImage, 6, {artX + half, artY + half})   // UV lower-right
//
// so the aperture is a square window 0.3/zoom of the art across, centred on the
// car -- 1126.6 world units at Bayview -- and the car sits at its centre by
// construction, which is why GetScreenPositionForWorldCoordinate only ever
// places *other* icons.
//
// Rotation. Tick reads the Explore Map Mode option byte (FEDatabase+0x5A,
// 0x803bc26a while exploring; +0x59, 0x803bc269 while racing) and rotates the
// map only when it reads 2. English.bin 0xc6269082 names it 'Explore Map Mode'
// with values Off (0) / Static (1) / Rotate (2). The default is NOT guessed:
// GameplaySettings::Default does `li r9, 2` and stores it to both +0x09 and
// +0x0A of its struct, and the live globals at FEDatabase+0x58..0x5C sit in the
// same order as that struct's +0x08..+0x0C (speed units, racing map, exploring
// map, jump cams), so retail ships both minimaps on Rotate.
//
// What this port does differently, and why
// ----------------------------------------
// Retail's minimap is a 128x128 inset in the corner of the race view. This one
// owns the bottom screen below the bank readout, which is the user's own layout
// call (CONTINUE.MD 1a #14: "the minimap takes up the rest of the screen below
// it"). Three consequences, each a deliberate deviation rather than a gap:
//
//   * The aperture is a square the size of the panel's short axis, not a
//     128-unit one: the panel is the bottom screen, so the circle inscribed in
//     it is far bigger than retail's inset (TASKS.md #287). The *scale* is
//     retail's -- one window, 0.3/zoom of art, across the diameter -- so the
//     bigger circle simply shows the same stretch of city larger. In rotate
//     mode the quad drawn is the square that covers that square at any angle
//     (hypot(w, h)), scissored to it, so no corner is ever empty.
//   * MINIMAP_MASK *is* drawn, as a blend: psMiniMapAlphaMask (0x801e6640)
//     sets up a two-stage TEV that multiplies its alpha into the map, and
//     dst *= src.alpha does the same thing here. That is what rounds the
//     panel. Its own alpha 204 is a translucency for compositing over the 3D
//     view, which this panel does not sit over, so the decode rescales the
//     peak back to opaque (texpack.h) and only the shape is taken.
//   * Of retail's icon pool the player's own, the shops, the career events,
//     the AI racers and the GPS icon are placed. Minimap_Icon_Start_Line
//     belongs to a race and Minimap_Icon_Cop_star to the showcase zones; an
//     icon with nothing behind it would be invented data, so those stay out
//     until what they mark exists.
//   * Retail pins an icon past its inset to a circle 0.55 windows out (see
//     "The GPS" below), which over its 128-unit widget is 1.1 times the
//     inset's own radius -- the icon straddles the ring, over the 3D view
//     outside it. There is no outside here: the circle is sized to the panel,
//     so that ring would fall off the screen's top and bottom edges. The pin
//     radius is the rim less the icon's half-size instead, which puts a pinned
//     icon against the ring's inner side rather than across it.
//
// Where a marker goes, and which art it uses
// ------------------------------------------
// Minimap::AddUnlockedTriggerZone (Speed.elf 0x80115B6C) is the whole chain.
// It takes the marker's world point as the midpoint of the zone record's own
// box -- ((bbMin + bbMax) * 0.5 on each axis, the +0x20/+0x24/+0x28/+0x2C
// fields trackpath.h documents) -- then asks GetTriggerZoneType
// (0x801289D0) what kind of trigger it is, maps that through
// _map_trigger_zone_type_to_element_type (0x80114E88) to a MinimapElementType,
// and hands both to AddStaticElement (0x80115A90).
//
// The art is then whatever the HUD screen authored for that element type.
// InitializeMinimapEntityList (0x80115008) builds the pool by looking up eight
// numbered widgets per type -- "PAINTSHOPINDICATOR_0".."_7" and so on, the
// names in MinimapEntityNames (0x8035B884) -- in HUD_SingleRace.fng itself,
// so each element type's texture is simply that widget's. Reading them out of
// the shipped InGameFreeRoam.bun copy of that screen gives:
//
//   element  1      STARTLINE          Minimap_Icon_Start_Line  0x059B3EC6
//   element  3,4    AIRACER, PLAYERCAR Minimap_Icon_Car         0xADA85247
//   element  5      SHOWCASE           Minimap_Icon_Cop_star    0x9B34E14C
//   element  6..11  CRIB and the five shops
//                                      Minimap_Icon_Safe_House  0x2CFD5273
//   element 12..18  the seven event types
//                                      Minimap_Icon_Circuit     0x25559384
//
// What tells them apart is colour. Every one of those widgets carries its own
// Color track (SA, B/G/R/A at +0x00), and the engine draws an Image widget's
// texels multiplied by it -- MinimapElement::Update never touches it, not even
// on the event types whose texture it swaps. The art is a white shape inside
// a black outline, so the widget colour is the marker's fill and the outline
// stays dark. Read out of the same .fng:
//
//   element  1  start line      (192,192,192)    20   x 20
//   element  3  AI racer        (207,128, 48)    12.5 x 16
//   element  4  player car      ( 48,207, 48)    12.5 x 16.5  (only _1 exists)
//   element  5  showcase        (210,195, 25)    14   x 14  (its "Static" script's;
//                               the widget's own is 190,180,45 -- see below)
//   element  6  crib            ( 91,  2,125)
//   element  7  paint shop      (114, 16, 29)
//   element  8  parts shop      ( 16,112, 16)
//   element  9  ICE shop        (128,128,  0)
//   element 10  performance     ( 43,100,164)
//   element 11  car lot         ( 82,163,163)
//   element 12  circuit event   (165, 60,233)
//   element 13  drag event      ( 50,130,200)
//   element 14  street event    (130,190,230)
//   element 15  drift event     (202,244,138)
//   element 16  sprint event    ( 14,156, 14)
//   element 17  URL event       (190,180, 45)
//   element 18  SUV event       (207,128, 48)
//
// all at alpha 255, and 6..18 all 14.5 x 14.5. (Update does lower an AI
// racer's alpha to 0x80 when that car is not leading its race --
// MinimapMarker.dim.)
//
// The blink (TASKS.md #241). Every time a trigger zone is unlocked,
// RecalcUnlockedZones (0x8012924C) has MinimapResyncBlink -> SyncBlink
// (0x80115964) set the zone's widget to the FE script its record's +0x14
// names: 0 "Static" (shops, showcases, world challenges), 1 "Normal" (career
// events), 2 "Dramatic" (no setter found). But in HUD_SingleRace.fng only
// the SHOWCASEINDICATOR_0..6 and AIRACERINDICATOR_0..7 widgets carry those
// three scripts, so FEngSetScript finds nothing on an event's or a shop's
// widget and they never blink; and the AI racers are not zones. What shows is
// the showcase's Static: colour (25,195,210) on disc, size 14 x 14 -- the
// colour in kElementColor. (Normal there would pulse 8 -> 12 -> 8 over 1.2 s,
// once; Dramatic grows 8 -> 24 and fades over 1.5 s, looping. Scripts are
// hashed by the upper-cased name: bStringHash("NORMAL") = 0x6EBBFB68.
// tools/re/wip241/blink_scripts.py dumps them.)
//
// So all six shop element types share one icon, and that is retail's own
// authoring rather than a simplification here: MinimapElement::Update
// (0x801156F4) switches on the element type and only types 12..18 reach its
// FEngSetTextureHash call (which swaps between Minimap_Icon_Circuit and
// Minimap_Icon_Cross, 0xB90F50BB, on the career record's +0x34). Types 6..11
// fall out of that switch untouched, so a shop keeps the texture the .fng gave
// it. Every one of those five hashes is in GLOBAL/InGameCommon.bun, the pack
// this file already opens.
//
// MINIMAP_BORDER is drawn, as HUD_SingleRace.fng authors it: one 128x128
// quarter of the ring in four copies at 0 / 90 / 180 / 270 degrees (guids
// 900976-900979). The art is centred on its own tile -- the opaque band runs
// from radius 41 to radius 64 of 128 -- so the four copies share one centre,
// the circle's, and the ring's outer edge is the tile inscribed, landing on
// the mask's rim. (Taking it for a corner bracket and putting a copy at each
// of a rectangle's corners is what TASKS.md #287 was raised about.)
// MINIMAP_BORDER_2, a darker inner band over radius 41..54, is in the same
// pack and is not drawn: which widget authors it is not decoded yet.
//
// The GPS (TASKS.md #286)
// -----------------------
// Minimap::GetScreenPositionForWorldCoordinate (0x8011668C) places every icon
// the same way: the icon's offset from the car in windows (0.3/zoom of the
// art), turned with the map. Past a limit the icon is not drawn (x = 512);
// inside it, past 0.55 windows, it is pinned at 0.55 on the same line. The
// limit is 1 window (0x80327B0C) while the GPS is idle in a career stage, and
// 10 (0x80327B1C) while it is engaged (GetGPSStatus 0..3) or the stage global
// 0x803BB288 reads 0. So with the GPS on, what it points at sits on the edge.
//
// MinimapElement::IsRenderable (0x801154D8) then keeps a trigger zone's icon
// only while it is the GPS's own (IsTriggerZoneTheGPSLocation) whenever the
// GPS is engaged and the stage is not 0: the target alone.
//
// MinimapWrapper::UpdateGPS (0x8010EEB8) owns three widgets of
// HUD_SingleRace.fng, bound in its constructor (0x8010EA90):
//
//   +0x30  0x697D2BCA  the GPS icon group (guid 937102): two
//                      Minimap_GPS_Selector rings (937103/937104) at the
//                      point GetMinimapArtCenter places -- GPSManager +0x10/
//                      +0x14, the zone's entry point -- through the
//                      placement above. Always set visible.
//   +0x24  GPS_Search_Group (933740): icon_Satellite, GPS_SEARCH_Text
//                      ('Searching^Connection') and GPS_SEARCH_ERROR. Shown
//                      while the status is 0..2, hidden on 3, and hidden while
//                      the player's follow (RealController +0x3EC) reads 1
//                      or 2, i.e. a path is still being followed.
//   +0x2C  GPS_SEARCH_ERROR: 0x155409E8 'Unable to contact satellite' on
//                      status 1, 0x31139B65 'Unable to find a path' on 2,
//                      otherwise hidden.
//
// and plays UI sound 0x12 on the frame the status first reads 3 (+0x38 holds
// the last frame's). The rings and the placement are here; the search group
// is text, which is main.c's, drawn over the panel's top-left corner.
//
// The rings run script 0x03826A28, 1600 ms, looping (flags 1), chained from
// each ring's initial 0x001744B3. Linear keys, base + delta, colour
// (255,128,0) and size 50 + delta:
//
//   ring 937103  alpha 0 / 255 / 180 / 0 / 0  at 0 / 200 / 600 / 1200 / 1600
//                size 28.75 / 37.83 / 56 / 70 / 70 at the same times
//   ring 937104  alpha 0 / 255 / 180 / 0      at 0 / 400 / 1000 / 1600
//                size 28.75 / 28.75 / 56 / 70 at the same times
//
// both 1 unit below the group's centre, and the group itself settles on
// script 0x036FB281: scale 0.8.

// Explore Map Mode. The byte at FEDatabase+0x5A; see above for the default.
typedef enum {
	MINIMAP_MODE_OFF = 0,
	MINIMAP_MODE_STATIC = 1,
	MINIMAP_MODE_ROTATE = 2,
} MinimapMode;

#define MINIMAP_MODE_DEFAULT MINIMAP_MODE_ROTATE

typedef struct Minimap Minimap;

// `mapPath` is the absolute path to TRACKS/TRACKMAP<trackId>.BIN and `hudBunPath`
// the one to GLOBAL/InGameCommon.bun, which is where Global\HudTexturesGlobal.tpk
// (and so every MINIMAP_* texture) lives. `freeRoamBunPath` is
// GLOBAL/InGameFreeRoam.bun, the only pack holding ICON_SATELLITE; NULL goes
// without it. `originX/originY/span/zoom` are the track record's
// +0xAC/+0xB0/+0xB4/+0xC0.
//
// Never returns NULL for a missing file: a Minimap whose map art did not load
// still draws its frame and reports minimap_have_map() false, so the caller can
// say which file is missing rather than showing an empty panel. (The map file
// WILL be missing on any card packed before this task -- TRACKS/TRACKMAP*.BIN
// was not in the asset tree.) Returns NULL only if the allocation itself fails.
Minimap* minimap_open(const char* mapPath, int trackId, const char* hudBunPath,
	const char* freeRoamBunPath, float originX, float originY, float span, float zoom,
	void (*log)(const char*));

void minimap_close(Minimap* m);

// False when TRACKMAP<id>.BIN could not be read -- the frame still draws.
bool minimap_have_map(const Minimap* m);

// The city art the panel has resident, and the track record's placement for
// it (+0xAC / +0xB0 / +0xB4). The in-game world map shows the same plate --
// its MAP_IMAGE is the same 0x90FE80F3 widget -- so it borrows this one rather
// than decode a second copy. False when no map is loaded, though the placement
// is still filled in whenever `m` is not NULL (`out` then has no texture).
bool minimap_map_art(const Minimap* m, C2D_Image* out, float* originX, float* originY,
	float* span);

// Retail's MinimapElementType, as MinimapEntityNames (Speed.elf 0x8035B884)
// pairs each widget-name pattern with one. Nothing names a type 2.
typedef enum {
	MINIMAP_ELEM_NONE = 0,
	MINIMAP_ELEM_STARTLINE = 1,
	MINIMAP_ELEM_AIRACER = 3,
	MINIMAP_ELEM_PLAYERCAR = 4,
	MINIMAP_ELEM_SHOWCASE = 5,
	MINIMAP_ELEM_CRIB = 6,
	MINIMAP_ELEM_PAINTSHOP = 7,
	MINIMAP_ELEM_PARTSSHOP = 8,
	MINIMAP_ELEM_ICESHOP = 9,
	MINIMAP_ELEM_PERFORMANCESHOP = 10,
	MINIMAP_ELEM_CARLOT = 11,
	MINIMAP_ELEM_CIRCUITEVENT = 12,
	MINIMAP_ELEM_DRAGEVENT = 13,
	MINIMAP_ELEM_STREETEVENT = 14,
	MINIMAP_ELEM_DRIFTEVENT = 15,
	MINIMAP_ELEM_SPRINTEVENT = 16,
	MINIMAP_ELEM_URLEVENT = 17,
	MINIMAP_ELEM_SUVEVENT = 18,
	MINIMAP_ELEM_COUNT
} MinimapElementType;

// One static marker, at a world point, drawn with one of the MINIMAP_ICON_*
// textures below. `texHash` is retail's own texture hash, so a caller names
// the art the way the engine does; a hash this file did not load draws
// nothing. `element` is the MinimapElementType AddStaticElement was handed,
// which is what picks the marker's colour (the table above).
typedef struct {
	float worldX, worldY;
	u32 texHash;
	u8 element;

	// Which way it faces, in the port's world yaw (0 = world +X, counter-
	// clockwise, the frame g_worldYaw uses). Only read when `turns` is set.
	//
	// Retail turns one icon and one only -- Minimap::Tick calls
	// SetScreenOrientation for the player's car and leaves every static
	// element upright -- which is right for a shop or an event start, since
	// those do not face anywhere. An AI racer's marker is the same car art as
	// the player's, though, so leaving it upright points the whole field
	// north however the cars are actually driving.
	bool  turns;
	float heading;

	// An AI racer behind the leader (TASKS.md #241): MinimapElement::Update
	// (Speed.elf 0x801156F4) gives a type-3 element whose car is a RacingCar
	// alpha 0xFF when that RacingCar's +10 is 1 and 0x80 otherwise -- +10 is
	// the place SortRacingCarsByLeadership (0x80229958) writes, 1 = leading.
	// A car that is not racing keeps its widget's 255. Zero, the default, is
	// drawn solid.
	bool  dim;
} MinimapMarker;

// The icons minimap.c loads for markers. Adding one means adding its hash to
// kTexHash there as well -- see the table in minimap.h's header comment for
// what each element type uses.
#define MINIMAP_ICON_SAFE_HOUSE 0x2CFD5273u

// Minimap_Icon_Car, 16x16 DXT3: the art both car element types use. The
// player's own car (element 4) is the arrow this file draws at the centre, so
// in practice a caller hands this to a marker for an AI racer (element 3),
// which is what colours it orange.
#define MINIMAP_ICON_CAR 0xADA85247u

// The two career-event icons. MinimapElement::Update (Speed.elf 0x801156F4)
// is the only place a marker's art is chosen in code rather than taken from
// the .fng widget: for element types 12..18 -- which is exactly the range a
// career event maps into -- it calls FEngSetTextureHash with CROSS when the
// event's class byte is 1 (SPON) and otherwise leaves the widget's own
// CIRCUIT art in place. Both are 16x16 DXT3 in InGameCommon.bun, the pack
// this file already opens.
#define MINIMAP_ICON_CIRCUIT 0x25559384u
#define MINIMAP_ICON_CROSS   0xB90F50BBu

// Minimap_Icon_Cop_star, 16x16 in InGameCommon.bun: element 5's art. Retail
// gives it to both kinds of zone RecalcUnlockedZones adds under that element,
// a showcase's trigger and a world challenge's start (#268 K).
#define MINIMAP_ICON_STAR 0x9B34E14Cu

// Swaps the map art for another track's, and with it the world-to-art
// placement. This is the race route: every Location4 record's TRACKMAP<id>.BIN
// is the same Bayview city plate with that track's route drawn on it in white
// and the rest of the city dimmed, which is why free roam uses 4000 (the one
// with no route) and a race uses its own. Retail does exactly this -- one
// SetTextureHash on widget 0x90FE80F3, see trackmap.h -- rather than drawing
// the route itself.
//
// Returns false and leaves the panel with no map when the file will not load;
// the caller logs nothing further, this does its own. Loading the track that
// is already up is a no-op that returns true.
bool minimap_set_track(Minimap* m, const char* mapPath, int trackId,
	float originX, float originY, float span, float zoom,
	void (*log)(const char*));

// What the placement and the GPS icon need of the GPS each frame (see "The
// GPS" above). The caller has already applied IsRenderable's target-only rule
// to the markers it hands over.
typedef struct {
	bool  engaged;   // GetGPSStatus reads 0..3
	bool  stageZero; // the stage global 0x803BB288 reads 0
	bool  icon;      // the GPS is started: draw the rings at iconX/iconY
	float iconX, iconY;
	u32   ms;        // a free-running millisecond clock for the rings' loop
} MinimapGps;

// Draws the circle inscribed in the rectangle (x, y, w, h) of the current
// citro2d target -- centred in it, with a diameter of the shorter side, which
// must be the bottom screen (the scissor is set in that target's coordinates).
// `headingRad` is the port's world yaw: 0 = facing world +X, increasing
// counter-clockwise, the same frame g_worldYaw and the telemetry line use.
// `markers`/`markerCount` are the static markers to place; pass NULL/0 for
// none. A marker past the placement's limit is skipped, so the caller can
// hand over the whole list every frame. `gps` NULL is the GPS idle.
// PORT (#471): the GPS rings' colour, 0xRRGGBB (retail 0x0080FF, the default).
void minimap_set_gps_colour(u32 rgb);

void minimap_draw(Minimap* m, float worldX, float worldY, float headingRad,
	float x, float y, float w, float h, MinimapMode mode,
	const MinimapMarker* markers, int markerCount, const MinimapGps* gps);

// ICON_SATELLITE (GPS_Search_Group's icon, guid 924606), for the caller that
// draws the group. False when InGameFreeRoam.bun did not give it.
bool minimap_satellite(const Minimap* m, C2D_Image* out);
