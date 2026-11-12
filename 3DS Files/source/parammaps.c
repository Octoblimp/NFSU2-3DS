#include "parammaps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See parammaps.h for the format. Chunk ids as the PC BUN stores them.
#define PM_GROUP_TYPE    0x8003B600u
#define PM_LAYER_TYPE    0x8003B601u
#define PM_HEADER_CHUNK  0x0003B602u
#define PM_TYPES_CHUNK   0x0003B603u
#define PM_OFFSETS_CHUNK 0x0003B604u
#define PM_ROWS_CHUNK    0x0003B605u
#define PM_QUAD8_CHUNK   0x0003B607u
#define PM_QUAD16_CHUNK  0x0003B608u

#define PM_HEADER_BYTES  0x24u
#define PM_MAX_LAYERS    16
#define PM_MAX_FIELDS    16
// Deeper than any shipped tree (Night Screen Tint's is 7 levels); only there
// so a bad child index cannot spin the lookup forever.
#define PM_MAX_DEPTH     24

struct ParamLayer {
	u32 hash;
	float x0, y0, x1, y1;
	int nodes, sets, stride, fields;
	u8 isInt[PM_MAX_FIELDS];
	int offs[PM_MAX_FIELDS];
	const u8* rows;
	const u8* quad;   // NULL: no tree, every point is set 0
	bool quad16;
};

struct ParamMaps {
	u8* blob;   // the group's payload, nothing else
	int count;
	ParamLayer layers[PM_MAX_LAYERS];
};

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static float rdf32le(const u8* p) {
	const u32 v = rd32le(p);
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

// bStringHash (Speed.elf), which is what a layer's header names it by.
static u32 stringHash(const char* s) {
	u32 h = 0xFFFFFFFFu;
	for (; *s; s++)
		h = h * 33u + (u8)*s;
	return h;
}

// One layer's sub-chunks into `l`. False for anything that does not hang
// together, which drops that layer and keeps the rest.
static bool parseLayer(ParamLayer* l, const u8* d, u32 len) {
	const u8 *hdr = NULL, *types = NULL, *offs = NULL, *rows = NULL, *quad = NULL;
	u32 rowsLen = 0, quadLen = 0, typesLen = 0, offsLen = 0;
	bool q16 = false;
	for (u32 o = 0; o + 8 <= len; ) {
		const u32 id = rd32le(d + o), n = rd32le(d + o + 4);
		if (n > len - o - 8)
			return false;
		const u8* p = d + o + 8;
		switch (id) {
		case PM_HEADER_CHUNK:  if (n >= PM_HEADER_BYTES) hdr = p; break;
		case PM_TYPES_CHUNK:   types = p; typesLen = n; break;
		case PM_OFFSETS_CHUNK: offs = p; offsLen = n; break;
		case PM_ROWS_CHUNK:    rows = p; rowsLen = n; break;
		case PM_QUAD8_CHUNK:   quad = p; quadLen = n; q16 = false; break;
		case PM_QUAD16_CHUNK:  quad = p; quadLen = n; q16 = true; break;
		default: break;
		}
		o += 8 + n;
	}
	if (!hdr || !rows)
		return false;
	memset(l, 0, sizeof(*l));
	l->hash   = rd32le(hdr);
	l->x0     = rdf32le(hdr + 0x04);
	l->y0     = rdf32le(hdr + 0x08);
	l->x1     = rdf32le(hdr + 0x0c);
	l->y1     = rdf32le(hdr + 0x10);
	l->nodes  = (int)rd32le(hdr + 0x14);
	l->sets   = (int)rd32le(hdr + 0x18);
	l->stride = (int)rd32le(hdr + 0x1c);
	l->fields = (int)rd32le(hdr + 0x20);
	if (l->sets <= 0 || l->stride <= 0 || l->fields <= 0 || l->fields > PM_MAX_FIELDS ||
	    (u32)l->sets * (u32)l->stride > rowsLen)
		return false;
	for (int f = 0; f < l->fields; f++) {
		const u32 at = (u32)f * 4;
		l->isInt[f] = types && at + 4 <= typesLen && rd32le(types + at) == 1;
		l->offs[f] = offs && at + 4 <= offsLen ? (int)rd32le(offs + at) : f * 4;
		if (l->offs[f] < 0 || l->offs[f] + 4 > l->stride)
			return false;
	}
	l->rows = rows;
	if (quad && l->nodes > 0 && (u32)l->nodes * (q16 ? 8u : 4u) <= quadLen) {
		l->quad = quad;
		l->quad16 = q16;
	}
	return true;
}

ParamMaps* parammaps_open(const char* path, void (*log)(const char* msg)) {
	char line[200];

	FILE* f = fopen(path, "rb");
	if (!f) {
		// world.c has already said the BUN is missing; this only names what
		// that costs here.
		if (log) {
			snprintf(line, sizeof(line), "parammaps: no %s -- per-area tint off", path);
			log(line);
		}
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	const long fileSize = ftell(f);

	// The flat {u32 type, u32 size} walk trackpath_open() does, reading
	// headers only until the group turns up; then just its payload.
	long payload = -1;
	u32 payloadSize = 0;
	for (long off = 0; off + 8 <= fileSize; ) {
		u8 hdr[8];
		if (fseek(f, off, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8)
			break;
		const u32 type = rd32le(hdr), size = rd32le(hdr + 4);
		if ((long)size > fileSize - (off + 8))
			break;
		if (type == PM_GROUP_TYPE) {
			payload = off + 8;
			payloadSize = size;
			break;
		}
		off += 8 + (long)size;
	}
	ParamMaps* pm = NULL;
	u8* blob = payload >= 0 && payloadSize >= 8 ? (u8*)malloc(payloadSize) : NULL;
	if (blob && fseek(f, payload, SEEK_SET) == 0 &&
	    fread(blob, 1, payloadSize, f) == payloadSize)
		pm = (ParamMaps*)calloc(1, sizeof(ParamMaps));
	fclose(f);
	if (!pm) {
		free(blob);
		if (log) {
			snprintf(line, sizeof(line), "parammaps: %s has no parameter maps", path);
			log(line);
		}
		return NULL;
	}
	pm->blob = blob;

	for (u32 o = 0; o + 8 <= payloadSize && pm->count < PM_MAX_LAYERS; ) {
		const u32 id = rd32le(blob + o), n = rd32le(blob + o + 4);
		if (n > payloadSize - o - 8)
			break;
		if (id == PM_LAYER_TYPE && parseLayer(&pm->layers[pm->count], blob + o + 8, n))
			pm->count++;
		o += 8 + n;
	}
	if (log) {
		snprintf(line, sizeof(line), "parammaps: %d layers, %u bytes", pm->count,
			(unsigned)payloadSize);
		log(line);
	}
	return pm;
}

void parammaps_close(ParamMaps* pm) {
	if (!pm)
		return;
	free(pm->blob);
	free(pm);
}

const ParamLayer* parammaps_layer(const ParamMaps* pm, const char* name) {
	if (!pm || !name)
		return NULL;
	const u32 h = stringHash(name);
	for (int i = 0; i < pm->count; i++)
		if (pm->layers[i].hash == h)
			return &pm->layers[i];
	return NULL;
}

int parammaps_set_count(const ParamLayer* l) {
	return l ? l->sets : 0;
}

int parammaps_field_count(const ParamLayer* l) {
	return l ? l->fields : 0;
}

static void readNode(const ParamLayer* l, int n, u32 c[4]) {
	if (l->quad16) {
		const u8* p = l->quad + n * 8;
		for (int k = 0; k < 4; k++)
			c[k] = (u32)p[k * 2] | ((u32)p[k * 2 + 1] << 8);
	} else {
		const u8* p = l->quad + n * 4;
		for (int k = 0; k < 4; k++)
			c[k] = p[k];
	}
}

// GetParameterSetIndexFromQuadData16 and its 8-bit twin (Speed.elf): the
// bounds test is strict, the split takes the midpoint as the high half.
int parammaps_set_at(const ParamLayer* l, float x, float y) {
	if (!l || !l->quad || !(x > l->x0 && x < l->x1 && y > l->y0 && y < l->y1))
		return 0;
	float lx = l->x0, ly = l->y0, hx = l->x1, hy = l->y1;
	int n = 0;
	for (int depth = 0; depth < PM_MAX_DEPTH; depth++) {
		u32 c[4];
		readNode(l, n, c);
		if (c[0] == 0)
			return (int)c[1] < l->sets ? (int)c[1] : 0;
		const float mx = (lx + hx) * 0.5f, my = (ly + hy) * 0.5f;
		u32 next;
		if (x >= mx) {
			lx = mx;
			if (y >= my) { ly = my; next = c[3]; }
			else         { hy = my; next = c[2]; }
		} else {
			hx = mx;
			if (y >= my) { ly = my; next = c[1]; }
			else         { hy = my; next = c[0]; }
		}
		if (next >= (u32)l->nodes)
			return 0;
		n = (int)next;
	}
	return 0;
}

float parammaps_field(const ParamLayer* l, int set, int field) {
	if (!l || set < 0 || set >= l->sets || field < 0 || field >= l->fields)
		return 0.0f;
	const u8* p = l->rows + set * l->stride + l->offs[field];
	return l->isInt[field] ? (float)(s32)rd32le(p) : rdf32le(p);
}

void paramaccessor_init(ParamAccessor* a, const ParamLayer* l) {
	memset(a, 0, sizeof(*a));
	a->layer = l;
}

void paramaccessor_capture(ParamAccessor* a, float x, float y, float blendDist) {
	const ParamLayer* l = a->layer;
	if (!l)
		return;
	const int set = parammaps_set_at(l, x, y);
	const int nf = l->fields < PARAM_ACCESSOR_FIELDS ? l->fields : PARAM_ACCESSOR_FIELDS;
	// BlendByDistance::CaptureData: t = 1 unless the point has a previous
	// position and moved less than blendDist since it.
	float t = 1.0f;
	if (a->captured && blendDist != 0.0f) {
		const float dx = x - a->lastX, dy = y - a->lastY;
		const float moved = sqrtf(dx * dx + dy * dy);
		if (moved < blendDist)
			t = moved / blendDist;
	}
	// Blend::CaptureData: the first capture copies the row.
	for (int f = 0; f < nf; f++) {
		const float target = parammaps_field(l, set, f);
		float v = a->captured ? target * t + a->values[f] * (1.0f - t) : target;
		if (l->isInt[f])
			v = floorf(v + 0.5f);
		a->values[f] = v;
	}
	a->lastX = x;
	a->lastY = y;
	a->captured = true;
}
