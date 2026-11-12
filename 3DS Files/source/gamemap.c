#include "gamemap.h"

#include <math.h>
#include <string.h>

// See gamemap.h. The decode is GameWorldMapScreen and WorldMapWrapper in
// Speed.elf, read through tools/gc_disasm.py.

// SectionInfoTable: {x, y, size} in the 512-unit art, the full map first, then
// one region per stage. The name hashes beside them are ZOOM_TEXT's.
static const float kSection[GAMEMAP_SECTIONS][3] = {
	{   0.0f,   0.0f, 512.0f }, // 0xD00905F2 "Full Map"
	{ 186.0f, 175.0f, 307.0f }, // 0xC08AF0B3
	{ 147.0f, 114.0f, 223.0f }, // 0xC08AF0B4
	{  23.0f,   0.0f, 233.0f }, // 0xC08AF0B5
	{ 251.0f, 299.0f, 213.0f }, // 0xC08AF0B6
	{ 132.0f, 341.0f, 171.0f }, // 0xC08AF0B7
};
#define GAMEMAP_ART 512.0f

// GetSectionInfoIndexFromStageNum: the last region a stage may zoom to.
static int sectionMaxForStage(int stage) {
	static const int k[7] = { 1, 1, 2, 3, 4, 5, 5 };
	if (stage < 0)
		stage = 0;
	if (stage > 6)
		stage = 6;
	return k[stage];
}

u8 gamemap_wme_for_zone_type(int zoneType) {
	// GetTranslatedZoneType.
	static const u8 k[GAMEMAP_FILTERS] = {
		GAMEMAP_WME_CIRCUIT, GAMEMAP_WME_DRAG, GAMEMAP_WME_STREET, GAMEMAP_WME_DRIFT,
		GAMEMAP_WME_SPRINT, GAMEMAP_WME_URL, GAMEMAP_WME_SHOWCASE, GAMEMAP_WME_SUV,
		GAMEMAP_WME_SHOWCASE, GAMEMAP_WME_PAINTSHOP, GAMEMAP_WME_PARTSSHOP,
		GAMEMAP_WME_ICESHOP, GAMEMAP_WME_PERFORMANCESHOP, GAMEMAP_WME_CARLOT,
		GAMEMAP_WME_CRIB, GAMEMAP_WME_SHOWCASE,
	};
	return (zoneType >= 0 && zoneType < GAMEMAP_FILTERS) ? k[zoneType] : 0xFF;
}

void gamemap_init(GameMap* m, unsigned char* filter, int careerStage,
		float originX, float originY, float span,
		float mapCx, float mapCy, float mapW, float mapH) {
	memset(m, 0, sizeof(*m));
	m->filter = filter;
	m->maxSection = sectionMaxForStage(careerStage);
	m->originX = originX;
	m->originY = originY;
	m->span = span;
	m->mapCx = mapCx;
	m->mapCy = mapCy;
	m->mapW = mapW;
	m->mapH = mapH;
	m->zoom = 1.0f;
	m->viewU = m->viewV = 0.5f;
	m->u1 = m->v1 = 1.0f;
	m->iconSize = 11.0f;
}

int gamemap_add(GameMap* m, float worldX, float worldY, int zoneType, bool hasEvent,
		bool sponsor, const void* zone, const int poolMax[GAMEMAP_WME_COUNT]) {
	const u8 wme = gamemap_wme_for_zone_type(zoneType);
	if (wme >= GAMEMAP_WME_COUNT || m->elemCount >= GAMEMAP_MAX_ELEMS)
		return -1;
	// The ctor's per-type counters start at 1, one widget per zone.
	int pool = 1;
	for (int i = 0; i < m->elemCount; i++)
		pool += m->elems[i].wme == wme;
	if (poolMax && pool > poolMax[wme])
		return -1;
	GameMapElem* e = &m->elems[m->elemCount];
	memset(e, 0, sizeof(*e));
	e->worldX = worldX;
	e->worldY = worldY;
	e->wme = wme;
	e->zoneType = (u8)zoneType;
	e->hasEvent = hasEvent;
	e->sponsor = sponsor;
	e->pool = pool;
	e->zone = zone;
	const float span = m->span > 0.0f ? m->span : 1.0f;
	e->au = (worldX - m->originX) / span;
	e->av = (m->originY - worldY) / span;
	e->onArt = e->au > 0.0f && e->au < 1.0f && e->av > 0.0f && e->av < 1.0f;
	return m->elemCount++;
}

bool gamemap_renderable(const GameMap* m, const GameMapElem* e) {
	return !m->filter || m->filter[e->zoneType] == 1;
}

bool gamemap_row_on(const GameMap* m, int r) {
	if (r < 0 || r >= m->rowCount)
		return false;
	return !m->filter || m->filter[m->rows[r].zoneType] == 1;
}

// SetupList. One slot per element type, its first zone's: a zone with an
// event record fills the front of the 16 in order, one without fills the back
// from slot 15 down. The used slots are then the rows, in slot order.
static void setupList(GameMap* m) {
	GameMapRow slots[GAMEMAP_SLOTS];
	bool used[GAMEMAP_SLOTS];
	int slotOf[GAMEMAP_WME_COUNT];
	memset(used, 0, sizeof(used));
	for (int t = 0; t < GAMEMAP_WME_COUNT; t++)
		slotOf[t] = -1;
	int front = 0, back = GAMEMAP_SLOTS - 1;
	for (int i = 0; i < m->elemCount; i++) {
		const GameMapElem* e = &m->elems[i];
		int s = slotOf[e->wme];
		if (s < 0) {
			if (front > back)
				continue;
			s = e->hasEvent ? front++ : back--;
			slotOf[e->wme] = s;
			used[s] = true;
			slots[s].wme = e->wme;
			slots[s].zoneType = e->zoneType;
			slots[s].sponsor = false;
		}
		if (e->sponsor)
			slots[s].sponsor = true;
	}
	m->rowCount = 0;
	for (int s = 0; s < GAMEMAP_SLOTS; s++) {
		if (used[s])
			m->rows[m->rowCount++] = slots[s];
	}
}

// BuildTriggerList for the highlighted row's type: CanUseGPS (every trigger
// element), IsRenderable, IsOnWorldMap and IsType, sorted down the map by
// screen y and across it by x.
//
// PORT: IsOnWorldMap and the sort are taken on the whole art rather than the
// view. Retail's view is one of six fixed regions, rebuilt on each step; the
// port's moves every frame, so the list keeps every marker of the type and
// the view follows the cursor instead (gamemap_show_cursor()). At the full
// map the two are the same list in the same order.
static void buildTriggerList(GameMap* m) {
	m->cursorCount = 0;
	if (m->highlight < 0 || m->highlight >= m->rowCount)
		return;
	const u8 wme = m->rows[m->highlight].wme;
	for (int i = 0; i < m->elemCount; i++) {
		const GameMapElem* e = &m->elems[i];
		if (e->wme != wme || !e->onArt || !gamemap_renderable(m, e))
			continue;
		int k = m->cursorCount++;
		while (k > 0) {
			const GameMapElem* p = &m->elems[m->cursor[k - 1]];
			if (p->av < e->av || (p->av == e->av && p->au <= e->au))
				break;
			m->cursor[k] = m->cursor[k - 1];
			k--;
		}
		m->cursor[k] = (short)i;
	}
}

void gamemap_rebuild_locator(GameMap* m, bool gpsIdle) {
	buildTriggerList(m);
	// RebuildGPSLocator starts on the list's first. Zoomed in, the port starts
	// on the one nearest the view's centre, so a new row does not throw the
	// view across the city.
	m->cursorAt = 0;
	if (m->zoom < 1.0f) {
		float best = 1e9f;
		for (int k = 0; k < m->cursorCount; k++) {
			const GameMapElem* e = &m->elems[m->cursor[k]];
			const float du = e->au - m->viewU, dv = e->av - m->viewV;
			const float d = du * du + dv * dv;
			if (d < best) {
				best = d;
				m->cursorAt = k;
			}
		}
	}
	m->locatorShown = gpsIdle && m->cursorCount > 0;
}

void gamemap_update_button(GameMap* m, bool gpsIdle) {
	m->gpsDisabled = gpsIdle && m->cursorCount == 0;
}

// WorldMapWrapper::Tick's placement. The region's square window over the art
// is (u - h, v - h) .. (u + h, v + h); a point's offset from the window's
// centre, in windows, scales onto MAP_IMAGE's rect. The window is the view's,
// which a region step sets to the region's own.
static void placeRaw(const GameMap* m, float worldX, float worldY, float* sx, float* sy) {
	const float u = m->viewU;
	const float v = m->viewV;
	const float f = m->zoom;
	const float span = m->span > 0.0f ? m->span : 1.0f;
	const float nx = ((worldX - m->originX) / span - u) / f;
	const float ny = ((m->originY - worldY) / span - v) / f;
	*sx = nx * m->mapW + m->mapCx;
	*sy = ny * m->mapH + m->mapCy;
}

static bool insideMap(const GameMap* m, float sx, float sy) {
	return sx > m->mapCx - m->mapW * 0.5f && sx < m->mapCx + m->mapW * 0.5f &&
		sy > m->mapCy - m->mapH * 0.5f && sy < m->mapCy + m->mapH * 0.5f;
}

bool gamemap_place(const GameMap* m, float worldX, float worldY, float* sx, float* sy) {
	placeRaw(m, worldX, worldY, sx, sy);
	return insideMap(m, *sx, *sy);
}

// The region the view is over, for ZOOM_TEXT: the full map while it shows
// the whole art; otherwise the smallest region whose window holds the view's
// centre, or failing that the one centred nearest it.
static int sectionAtView(const GameMap* m) {
	if (m->zoom >= 1.0f)
		return 0;
	int best = -1, near = 1;
	float bestSize = 2.0f, nearD = 1e9f;
	for (int i = 1; i < GAMEMAP_SECTIONS; i++) {
		const float* s = kSection[i];
		const float x0 = s[0] / GAMEMAP_ART, y0 = s[1] / GAMEMAP_ART, f = s[2] / GAMEMAP_ART;
		if (m->viewU >= x0 && m->viewU <= x0 + f && m->viewV >= y0 && m->viewV <= y0 + f &&
				f < bestSize) {
			best = i;
			bestSize = f;
		}
		const float du = x0 + f * 0.5f - m->viewU, dv = y0 + f * 0.5f - m->viewV;
		if (du * du + dv * dv < nearD) {
			nearD = du * du + dv * dv;
			near = i;
		}
	}
	return best >= 0 ? best : near;
}

void gamemap_tick(GameMap* m, bool gpsIdle) {
	const float u = m->viewU;
	const float v = m->viewV;
	const float f = m->zoom;
	const float h = f * 0.5f;
	m->u0 = u - h;
	m->v0 = v - h;
	m->u1 = u + h;
	m->v1 = v + h;
	if (!m->sectionPinned)
		m->section = sectionAtView(m);
	// Every element, the player's included, at 11 on the full map and 15 in a
	// region; the port's view goes from one to the other over the zoom down to
	// the largest region (307 of 512).
	const float big = kSection[1][2] / GAMEMAP_ART;
	const float t = f >= 1.0f ? 0.0f : f <= big ? 1.0f : (1.0f - f) / (1.0f - big);
	m->iconSize = 11.0f + 4.0f * t;
	for (int i = 0; i < m->elemCount; i++) {
		GameMapElem* e = &m->elems[i];
		placeRaw(m, e->worldX, e->worldY, &e->sx, &e->sy);
		e->onMap = insideMap(m, e->sx, e->sy);
	}
	if (m->rebuild) {
		m->rebuild = false;
		gamemap_rebuild_locator(m, gpsIdle);
	}
}

void gamemap_setup(GameMap* m) {
	setupList(m);
	// StartFiltering: the first row highlighted, if there is one.
	m->highlight = 0;
	// StartGPSLocator: the list waits for the first tick's placement.
	m->rebuild = true;
	m->section = 0;
	m->sectionPinned = true;
	m->zoom = 1.0f;
	m->viewU = m->viewV = 0.5f;
	m->cursorCount = 0;
	m->locatorShown = false;
}

bool gamemap_scroll_highlight(GameMap* m, int dir, bool gpsIdle) {
	const int to = m->highlight + (dir < 0 ? -1 : 1);
	if (to < 0 || to >= m->rowCount)
		return false;
	m->highlight = to;
	gamemap_rebuild_locator(m, gpsIdle);
	return true;
}

bool gamemap_toggle_filter(GameMap* m, bool gpsIdle) {
	if (m->rowCount <= 0 || m->highlight < 0 || m->highlight >= m->rowCount || !m->filter)
		return false;
	const u8 wme = m->rows[m->highlight].wme;
	const unsigned char on = gamemap_row_on(m, m->highlight) ? 0 : 1;
	// Every zone type that translates to the row's element type: one for all
	// but the showcase, whose three (6, 8, 15) go together.
	for (int t = 0; t < GAMEMAP_FILTERS; t++) {
		if (gamemap_wme_for_zone_type(t) == wme)
			m->filter[t] = on;
	}
	gamemap_rebuild_locator(m, gpsIdle);
	gamemap_update_button(m, gpsIdle);
	return true;
}

// The view inside the art: its centre at least half a window from each edge.
static void clampView(GameMap* m) {
	if (m->zoom > 1.0f)
		m->zoom = 1.0f;
	if (m->zoom < GAMEMAP_ZOOM_MIN)
		m->zoom = GAMEMAP_ZOOM_MIN;
	const float h = m->zoom * 0.5f;
	if (m->viewU < h) m->viewU = h;
	if (m->viewU > 1.0f - h) m->viewU = 1.0f - h;
	if (m->viewV < h) m->viewV = h;
	if (m->viewV > 1.0f - h) m->viewV = 1.0f - h;
}

void gamemap_scroll_zoom(GameMap* m, int dir) {
	// A region past the stage's last (the view can be panned over one) steps
	// as if from the last.
	int idx = m->section > m->maxSection ? m->maxSection : m->section;
	if (dir > 0) {
		idx++;
	} else {
		idx--;
		if (idx < 0)
			idx = m->maxSection;
	}
	idx %= (m->maxSection + 1);
	m->section = idx;
	m->sectionPinned = true;
	const float* s = kSection[idx];
	m->zoom = s[2] / GAMEMAP_ART;
	m->viewU = (s[0] + s[2] * 0.5f) / GAMEMAP_ART;
	m->viewV = (s[1] + s[2] * 0.5f) / GAMEMAP_ART;
	clampView(m);
}

bool gamemap_zoom_by(GameMap* m, float factor) {
	const float z0 = m->zoom, u0 = m->viewU, v0 = m->viewV;
	m->zoom *= factor;
	clampView(m);
	if (m->zoom == z0 && m->viewU == u0 && m->viewV == v0)
		return false;
	m->sectionPinned = false;
	return true;
}

bool gamemap_pan(GameMap* m, float du, float dv) {
	const float u0 = m->viewU, v0 = m->viewV;
	m->viewU += du * m->zoom;
	m->viewV += dv * m->zoom;
	clampView(m);
	if (m->viewU == u0 && m->viewV == v0)
		return false;
	m->sectionPinned = false;
	return true;
}

bool gamemap_show_cursor(GameMap* m) {
	const GameMapElem* e = gamemap_cursor(m);
	if (!e || m->zoom >= 1.0f)
		return false;
	// Inside the window with a tenth of it to spare: left where it is.
	const float in = m->zoom * 0.4f;
	if (fabsf(e->au - m->viewU) <= in && fabsf(e->av - m->viewV) <= in)
		return false;
	m->viewU = e->au;
	m->viewV = e->av;
	clampView(m);
	// The view has left the stepped region, so ZOOM_TEXT names where it is now.
	m->sectionPinned = false;
	return true;
}

bool gamemap_scroll_gps(GameMap* m, int dir) {
	if (m->cursorCount <= 0)
		return false;
	if (dir < 0)
		m->cursorAt = m->cursorAt > 0 ? m->cursorAt - 1 : m->cursorCount - 1;
	else
		m->cursorAt = m->cursorAt + 1 < m->cursorCount ? m->cursorAt + 1 : 0;
	return m->cursorCount > 1;
}

bool gamemap_cursor_to(GameMap* m, int e) {
	for (int k = 0; k < m->cursorCount; k++) {
		if (m->cursor[k] == e) {
			m->cursorAt = k;
			return true;
		}
	}
	return false;
}

const GameMapElem* gamemap_cursor(const GameMap* m) {
	if (m->cursorCount <= 0 || m->cursorAt < 0 || m->cursorAt >= m->cursorCount)
		return NULL;
	return &m->elems[m->cursor[m->cursorAt]];
}
