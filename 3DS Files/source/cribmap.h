#pragma once
#include <3ds.h>
#include "eventdb.h"
#include "trackdb.h"
#include "profile_save.h"

// CribWorldMapScreen (UI_CareerWorldMap.fng), the crib's WORLD MAP -- TASKS.md
// #259 item 7. The decode, with every address, is
// docs/formats/crib-world-map-re.md; this is the screen's model, kept apart
// from main.c's widgets so that what retail computes is readable on its own:
//
//   * the region list (Scrollerina) -- one "Stage %d" per completed stage;
//   * the region's events (SetupRegionEvents / BuildEventList), the region
//     node first;
//   * the vertical icon strip (ThumbnailScroller): its spline, which nodes
//     show, their size and alpha;
//   * the 0.3 s settle after a scroll, then the map (TrackMapScroller): which
//     TRACKMAP<id> is wanted and the zoom window it closes in on.
//
// main.c owns the widgets, the strings, the texture load and the pad.

// BuildEventList (PC 0x4B1360) stops once more than 0x46 have been added.
#define CRIBMAP_MAX_EVENTS 71
#define CRIBMAP_MAX_NODES (CRIBMAP_MAX_EVENTS + 1)

// MINI_MASTER, the strip's origin, and AddNode's stride: node height 16 plus
// the scroller's spacing (+0x60) 20.
#define CRIBMAP_MASTER_X   (-264.0f)
#define CRIBMAP_MASTER_Y   24.0f
#define CRIBMAP_NODE_SIZE  16.0f
#define CRIBMAP_NODE_STEP  36.0f
// MoveObjectsTo's window, 273 canvas units centred on the master, and the
// size every node but the current one draws at (0.8 x 16).
#define CRIBMAP_WINDOW     273.0f
#define CRIBMAP_SMALL_SIZE 12.8f

// SideScrollBar_Backing's extent (8x290 at y 22) and its handle's length.
#define CRIBMAP_SIDEBAR_TOP    (-123.0f)
#define CRIBMAP_SIDEBAR_LEN    290.0f
#define CRIBMAP_SIDEBAR_HANDLE 16.0f

// The selection has to sit this long before the map follows it (0x78758C).
#define CRIBMAP_SETTLE_MS 300

typedef struct {
	const EventRec* ev;  // NULL on the region node
	s8 type;             // event +0x0F, an EventType
	u8 kind;             // the first track entry's reversed bit (0x4FFDC0)
	bool completed;      // HasCompletedEvent: state record == 1
	s32 best;            // drift: points; otherwise 1/4000 s ticks
	u32 distanceM;       // TrackInfo +0x98
	// The SelectableTrack (PlayableEvent ctor 0x4B1200, PlayableRegion
	// 0x4E0B50): the map it shows and the window ZoomIn closes in on.
	int mapId;
	float centreX, centreY, zoom;
	bool snap;
	// What MoveObjectsTo last made of the node.
	float drawY;         // canvas centre y
	bool shown;
	u8 alpha;
} CribMapNode;

typedef struct {
	int regionCount;     // regions listed: stages 1.. while completed
	int region;          // the current one, 1-based
	int nodeCount;
	int current;         // 0-based; node 0 is the region
	CribMapNode nodes[CRIBMAP_MAX_NODES];
	int poolCount;       // EVENT_ICONS widgets the package carries

	// ThumbnailScroller's spline (SplineSeek 0x574E30) and its two bytes:
	// +0x31 visibility and +0x2F fade.
	float pos, vel, target;
	float sa, sb, sc, sd, st;
	int splineState;     // 0 idle, 1 running, 2 restart
	bool visible, fade;

	// The screen's selection timer (+0x22C): set on every move, cleared
	// once SetTrackMap has run for the settled node.
	bool selPending;
	u64 selMs;

	// TrackMapScroller (0x4CDD40). mapLoaded follows mapWanted once main.c
	// has the texture up and calls cribmap_map_loaded().
	int mapWanted, mapLoaded;
	float mapCx, mapCy, mapZoom;
	bool mapSnap;
	int zoomTicks;       // 1/4000 s
	bool zoomSnapPending;
	float u0, v0, u1, v1; // ZoomIn's window over the art, 0..1
} CribMap;

// Builds the screen as the ctor and the first SetupRegionEvents do. `alpha`
// is the EVENT_ICONS widgets' authored alpha, which the nodes keep until the
// fade byte is first set. `poolCount` caps the nodes at the pooled widgets.
void cribmap_init(CribMap* m, const EventDb* events, const TrackDb* tracks,
	const Profile* p, int poolCount, u8 alpha, u64 nowMs);

// ScrollEvent (0x4CC140) and ScrollRegion (0x4E0E50), `dir` -1 or +1. True
// when the selection moved, which is the only case retail plays a sound.
bool cribmap_scroll_event(CribMap* m, int dir, u64 nowMs, float dt);
bool cribmap_scroll_region(CribMap* m, const EventDb* events, const TrackDb* tracks,
	const Profile* p, int dir, u64 nowMs, float dt);

// Jumps straight to node `k` (the port's tap on an icon): Scroll's own steps,
// repeated until the current node is `k`.
bool cribmap_select_node(CribMap* m, int k, u64 nowMs, float dt);

// StartDim / StopDim: the dialog's way in and out.
void cribmap_start_dim(CribMap* m);
void cribmap_stop_dim(CribMap* m);

// The screen's per-frame tick (0xC98356BA): the strip, the settle and
// SetTrackMap, and TrackMapScroller::Update.
void cribmap_tick(CribMap* m, u64 nowMs, float dt);

// The async load's completion: the wanted map is resident.
void cribmap_map_loaded(CribMap* m);

// SideScrollBar_Handle's centre y (0x52E320).
float cribmap_side_handle_y(const CribMap* m);

// RefreshHeader's arrows: `end` 0 is the head end (left / up), 1 the tail.
bool cribmap_region_arrow_shown(const CribMap* m, int end);
bool cribmap_event_arrow_shown(const CribMap* m, int end);
// FEScrollBar::Update(1, count, index) shows the handle only past one item.
bool cribmap_side_handle_shown(const CribMap* m);

// The node the screen's A would act on, NULL on the region node.
const EventRec* cribmap_current_event(const CribMap* m);
