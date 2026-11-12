#include "cribmap.h"
#include <math.h>
#include <string.h>

// The two DDAY records BuildEventList leaves out by id.
#define CRIBMAP_DDAY_A 0xDD60E402u
#define CRIBMAP_DDAY_B 0xDD60E403u

// StageMapInfoTable (SPEED2.EXE 0x7F4388, stride 0x14): what PlayableRegion
// (0x4E0B50) shows for a region -- the window over the art, the map, and the
// snap byte (zero on all six).
typedef struct {
	float cx, cy, zoom;
	int trackId;
	bool snap;
} StageMapInfo;

static const StageMapInfo STAGE_MAP_INFO[] = {
	{   0.0f,   0.0f, 512.0f, 4000, false },
	{ 163.0f, 154.0f, 350.0f, 4500, false },
	{ 126.0f,  93.0f, 387.0f, 4501, false },
	{  17.0f,   5.0f, 480.0f, 4502, false },
	{   0.0f,   0.0f, 512.0f, 4503, false },
	{   0.0f,   0.0f, 512.0f, 4504, false },
};
#define STAGE_MAP_INFO_COUNT ((int)(sizeof(STAGE_MAP_INFO) / sizeof(STAGE_MAP_INFO[0])))

// HasCompletedEvent (Speed.elf 0x801271DC): the state record must exist and
// read exactly 1.
static bool eventCompleted(const Profile* p, u32 eventId) {
	if (!p || !p->isLoaded)
		return false;
	for (u32 k = 0; k < p->stateRecCount && k < (u32)PROFILE_NUM_STATE_RECORDS; k++)
		if (p->stateRec[k].eventId == eventId)
			return p->stateRec[k].state == 1;
	return false;
}

static bool stageCompleted(const Profile* p, int s) {
	return p && p->isLoaded && s >= 0 && s < PROFILE_NUM_STAGES && p->stageCompleted[s];
}

// The ramp MoveObjectsTo fades a node through at either end of the window:
// 0 outside it, rising over `ramp` from each edge, 1 in between.
static float thumbScale(float v, float c, float ext, float ramp) {
	const float lo = c - ext * 0.5f;
	const float hi = c + ext * 0.5f;
	if (v < lo || v > hi)
		return 0.0f;
	if (v < lo + ramp)
		return (v - lo) / ramp;
	if (v > hi - ramp)
		return (hi - v) / ramp;
	return 1.0f;
}

// MoveObjectsTo (0x52ED80): each node's centre from the strip position, shown
// only inside the window and while the visibility byte is set. The fade byte
// rewrites a shown node's alpha; a node outside the window is only hidden,
// its colour left as it was (GC; PC's own arm zeroes it, see the RE doc).
static void moveObjectsTo(CribMap* m, float pos) {
	for (int k = 0; k < m->nodeCount; k++) {
		CribMapNode* n = &m->nodes[k];
		n->drawY = pos + CRIBMAP_NODE_STEP * (float)k + CRIBMAP_MASTER_Y;
		n->shown = m->visible && fabsf(n->drawY - CRIBMAP_MASTER_Y) <= CRIBMAP_WINDOW * 0.5f;
		if (n->shown && m->fade) {
			const float s = thumbScale(n->drawY, CRIBMAP_MASTER_Y, CRIBMAP_WINDOW,
				CRIBMAP_SMALL_SIZE);
			n->alpha = (u8)(int)(255.0f * s);
		}
	}
}

// SplineSeek (0x574E30): a cubic Hermite from the current position and
// velocity to the target at rest, over 0.3 s.
static void splineSeek(CribMap* m, float dt) {
	if (m->splineState == 2) {
		const float d = m->target - m->pos;
		m->st = 0.0f;
		m->splineState = 1;
		m->sd = m->pos;
		m->sc = m->vel;
		m->sa = 0.0f + m->vel - 2.0f * d;
		m->sb = 3.0f * d - 0.0f - 2.0f * m->vel;
	}
	if (m->splineState != 1)
		return;
	m->st += dt / 0.3f;
	if (m->st < 1.0f) {
		const float t = m->st;
		m->pos = ((m->sa * t + m->sb) * t + m->sc) * t + m->sd;
		m->vel = (2.0f * m->sb + 3.0f * m->sa * t) * t + m->sc;
	} else {
		m->splineState = 0;
		m->pos = m->target;
		m->vel = 0.0f;
	}
}

// ThumbnailScroller::Update (0x538760): the nodes go where the strip is now,
// then the spline steps -- so what draws is the pre-step position.
static void thumbUpdate(CribMap* m, float dt) {
	moveObjectsTo(m, m->pos);
	splineSeek(m, dt);
}

// SnapToHead (0x538740 -> 0x5386A0).
static void snapToHead(CribMap* m) {
	m->current = 0;
	m->target = 0.0f;
	m->pos = 0.0f;
	m->vel = 0.0f;
	m->splineState = 0;
	moveObjectsTo(m, m->pos);
}

// Scroll (0x5437C0): one node along, the spline aimed at it.
static bool scroll(CribMap* m, int dir) {
	const int next = m->current + dir;
	if (next < 0 || next >= m->nodeCount)
		return false;
	m->current = next;
	m->target = -CRIBMAP_NODE_STEP * (float)next;
	if (m->target != m->pos)
		m->splineState = 2;
	return true;
}

// The region item and BuildEventList (PC 0x4B1360), in table order.
static void buildNodes(CribMap* m, const EventDb* events, const TrackDb* tracks,
	const Profile* p)
{
	const int cap = m->poolCount < CRIBMAP_MAX_NODES ? m->poolCount : CRIBMAP_MAX_NODES;
	m->nodeCount = 0;
	if (cap <= 0)
		return;

	CribMapNode* rn = &m->nodes[m->nodeCount++];
	const u8 keepAlpha = rn->alpha;
	memset(rn, 0, sizeof(*rn));
	rn->alpha = keepAlpha;
	const StageMapInfo* si = &STAGE_MAP_INFO[
		m->region >= 0 && m->region < STAGE_MAP_INFO_COUNT ? m->region : 0];
	rn->type = -1;
	rn->mapId = si->trackId;
	rn->centreX = si->cx;
	rn->centreY = si->cy;
	rn->zoom = si->zoom;
	rn->snap = si->snap;

	if (!events || !tracks || !stageCompleted(p, m->region))
		return;
	int added = 0;
	for (int i = 0; i < events->count && m->nodeCount < cap; i++) {
		const EventRec* ev = &events->recs[i];
		if (!(ev->stage == m->region || (m->region == 1 && ev->stage == 0)))
			continue;
		if (ev->trackCount <= 0)
			continue;
		const TrackRec* r = trackdb_find(tracks, (u16)ev->tracks[0].trackId);
		if (!r || ev->eventId == CRIBMAP_DDAY_A || ev->eventId == CRIBMAP_DDAY_B)
			continue;
		CribMapNode* n = &m->nodes[m->nodeCount++];
		const u8 a = n->alpha;
		memset(n, 0, sizeof(*n));
		n->alpha = a;
		n->ev = ev;
		n->type = ev->type;
		n->kind = ev->tracks[0].flags & 1;
		n->completed = eventCompleted(p, ev->eventId);
		if (ev->type == EVENT_TYPE_DRIFT) {
			int s = p && p->isLoaded ? profile_best_score(p, r->trackId, n->kind) : 0;
			n->best = s != 0 ? s : trackdb_best_score(r, n->kind);
		} else {
			int t = 0;
			n->best = (p && p->isLoaded && profile_best_time(p, r->trackId, n->kind, &t))
				? t : trackdb_best_time_ticks(r, n->kind);
		}
		n->distanceM = r->distanceM;
		n->mapId = r->trackId;
		n->centreX = r->selCentreX;
		n->centreY = r->selCentreY;
		n->zoom = r->selZoom;
		n->snap = r->selSnap != 0;
		if (++added > 0x46)
			break;
	}
}

// SetupRegionEvents (0x4E0CE0).
static void setupRegionEvents(CribMap* m, const EventDb* events, const TrackDb* tracks,
	const Profile* p, float dt)
{
	buildNodes(m, events, tracks, p);
	snapToHead(m);
	m->visible = true;
	thumbUpdate(m, dt);
}

// ZoomIn (0x498630): the window eases from the whole art to the node's.
static void zoomIn(CribMap* m, float t) {
	const float s = sinf(t * 1.5707964f);
	const float cx = m->mapCx * s;
	const float cy = m->mapCy * s;
	const float z = 512.0f - (512.0f - m->mapZoom) * s;
	m->u0 = cx / 512.0f;
	m->v0 = cy / 512.0f;
	m->u1 = (cx + z) / 512.0f;
	m->v1 = (cy + z) / 512.0f;
}

// SetTrackMap: the node's map is the wanted one from here on.
static void setTrackMap(CribMap* m, const CribMapNode* n) {
	m->mapWanted = n->mapId;
	m->mapCx = n->centreX;
	m->mapCy = n->centreY;
	m->mapZoom = n->zoom;
	m->mapSnap = n->snap;
}

// 0x577310: counts the timer down by the frame, true while it still runs.
static bool countdown(int* ticks, float dt) {
	*ticks -= (int)(dt * 4000.0f);
	if (*ticks < 0) {
		*ticks = 0;
		return false;
	}
	return *ticks != 0 && *ticks != 0x7FFFFFFF;
}

// TrackMapScroller::Update, once the wanted map is the resident one.
static void trackMapUpdate(CribMap* m, float dt) {
	if (m->mapWanted < 0 || m->mapLoaded != m->mapWanted)
		return;
	if (m->zoomSnapPending) {
		zoomIn(m, 1.0f);
		m->zoomSnapPending = false;
		return;
	}
	if (countdown(&m->zoomTicks, dt)) {
		const float f = (float)m->zoomTicks * 0.00025f;
		if (f <= 0.5f)
			zoomIn(m, 1.0f - f / 0.5f);
	}
}

void cribmap_init(CribMap* m, const EventDb* events, const TrackDb* tracks,
	const Profile* p, int poolCount, u8 alpha, u64 nowMs)
{
	memset(m, 0, sizeof(*m));
	m->poolCount = poolCount;
	for (int k = 0; k < CRIBMAP_MAX_NODES; k++)
		m->nodes[k].alpha = alpha;
	// buildRegions (0x4CBF80): stage 1 upwards while completed. The initial
	// region byte is 1 whether or not anything was listed.
	for (int s = 1; s < PROFILE_NUM_STAGES && stageCompleted(p, s); s++)
		m->regionCount++;
	m->region = 1;
	m->mapWanted = -1;
	m->mapLoaded = -1;
	m->u0 = m->v0 = 0.0f;
	m->u1 = m->v1 = 1.0f;
	setupRegionEvents(m, events, tracks, p, 0.0f);
	m->selPending = true;
	m->selMs = nowMs;
}

bool cribmap_scroll_event(CribMap* m, int dir, u64 nowMs, float dt) {
	const int before = m->current;
	if (!scroll(m, dir))
		return false;
	if (m->current != before) {
		m->selPending = true;
		m->selMs = nowMs;
	}
	thumbUpdate(m, dt);
	return true;
}

bool cribmap_select_node(CribMap* m, int k, u64 nowMs, float dt) {
	if (k < 0 || k >= m->nodeCount || k == m->current)
		return false;
	const int dir = k > m->current ? 1 : -1;
	while (m->current != k && scroll(m, dir)) {
	}
	m->selPending = true;
	m->selMs = nowMs;
	thumbUpdate(m, dt);
	return true;
}

bool cribmap_scroll_region(CribMap* m, const EventDb* events, const TrackDb* tracks,
	const Profile* p, int dir, u64 nowMs, float dt)
{
	const int next = m->region + dir;
	if (m->regionCount <= 0 || next < 1 || next > m->regionCount)
		return false;
	m->region = next;
	m->selPending = true;
	m->selMs = nowMs;
	setupRegionEvents(m, events, tracks, p, dt);
	return true;
}

void cribmap_start_dim(CribMap* m) {
	m->fade = false;
	m->visible = false;
	moveObjectsTo(m, m->pos);
}

void cribmap_stop_dim(CribMap* m) {
	m->fade = true;
	m->visible = true;
	moveObjectsTo(m, m->pos);
}

void cribmap_tick(CribMap* m, u64 nowMs, float dt) {
	thumbUpdate(m, dt);
	if (m->selPending) {
		if (nowMs - m->selMs >= CRIBMAP_SETTLE_MS) {
			if (m->current >= 0 && m->current < m->nodeCount)
				setTrackMap(m, &m->nodes[m->current]);
			m->selPending = false;
		}
		return;
	}
	trackMapUpdate(m, dt);
}

// The load callback and InitZoom (0x4B4B80): no window -> the whole art;
// otherwise hold the whole art for 0.5 s and close in over the next 0.5 s, or
// jump there when the track snaps.
void cribmap_map_loaded(CribMap* m) {
	m->mapLoaded = m->mapWanted;
	if (m->mapZoom == 0.0f) {
		m->zoomTicks = 0;
		zoomIn(m, 0.0f);
	} else if (!m->mapSnap) {
		m->zoomTicks = 4000;
		m->zoomSnapPending = false;
		zoomIn(m, 0.0f);
	} else {
		m->zoomSnapPending = true;
		m->zoomTicks = 0;
	}
	if (m->mapSnap)
		zoomIn(m, 1.0f);
}

float cribmap_side_handle_y(const CribMap* m) {
	const int count = m->nodeCount;
	const int i = m->current;
	const float half = CRIBMAP_SIDEBAR_HANDLE * 0.5f;
	const float step = (CRIBMAP_SIDEBAR_LEN - CRIBMAP_SIDEBAR_HANDLE) /
		(float)(count - 1 > 1 ? count - 1 : 1);
	if (i == 0)
		return CRIBMAP_SIDEBAR_TOP + half;
	if (i == count - 1)
		return CRIBMAP_SIDEBAR_TOP + CRIBMAP_SIDEBAR_LEN - half;
	return CRIBMAP_SIDEBAR_TOP + half + step * (float)i;
}

bool cribmap_region_arrow_shown(const CribMap* m, int end) {
	if (m->regionCount <= 1)
		return false;
	return end == 0 ? m->region > 1 : m->region < m->regionCount;
}

bool cribmap_event_arrow_shown(const CribMap* m, int end) {
	if (m->nodeCount <= 1)
		return false;
	return end == 0 ? m->current > 0 : m->current < m->nodeCount - 1;
}

bool cribmap_side_handle_shown(const CribMap* m) {
	return m->nodeCount > 1;
}

const EventRec* cribmap_current_event(const CribMap* m) {
	if (m->current < 0 || m->current >= m->nodeCount)
		return NULL;
	return m->nodes[m->current].ev;
}
