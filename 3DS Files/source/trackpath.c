#include "trackpath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See trackpath.h for where the file lives and where every offset came from.
#define TP_CONTAINER_TYPE 0x80034147u // TrackPathManager::Loader's magic
#define TP_ZONE_CHUNK     0x0003414Au // the zone list
#define TP_MAX_ZONES      4096        // the shipped L4RA file has 671

// The record is read in place, as the engine reads it, so the struct has to be
// the file's own layout rather than whatever the compiler would have picked.
_Static_assert(sizeof(TrackPathZone) == TPZ_HEADER_BYTES,
	"TrackPathZone must match the shipped record head");

// Speed.elf 0x8036FD14. The order is the enum, and FixupPointers' loop bound
// (`cmpwi r10, 0x14`) is what says the enum stops at 20.
static const char* const kTypeNames[TPZ_TYPE_COUNT] = {
	"NO_WRONG_WAY", "WRONG_WAY", "RESET", "RESET_TO_POINT", "GUIDED_RESET",
	"TUNNEL", "OVERPASS", "OVERPASS_SMALL", "DRIFT_GATE", "DRIFT_HOT_SPOT",
	"TUTORIAL", "SHORTCUT", "STREAMER_PREDICTION", "RACE_START_TRIGGER",
	"SHOP_TRIGGER", "SHOWCASE_TRIGGER", "TRAFFIC_TYPE", "NEIGHBOURHOOD",
	"SMS_TRIGGER", "BANK_TRIGGER", "GARAGE"
};

const char* trackpath_type_name(u32 type) {
	return type < TPZ_TYPE_COUNT ? kTypeNames[type] : NULL;
}

struct TrackPaths {
	u8* blob;   // the 0x3414A payload, nothing else
	u32 bytes;
	int count;
	// TrackPathManager::FixupPointers' +0x448 table: the first zone of each
	// type, with the list's end in the extra slot so FindZone's [type+1] read
	// needs no special case for the last type.
	const u8* typeBegin[TPZ_TYPE_COUNT + 1];
	int typeCount[TPZ_TYPE_COUNT];
};

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// bBoundingBoxIsInside (Speed.elf 0x8005D76C), with retail's own slack of 0 at
// the one call site that matters (FindZone passes 0.0f).
static bool boxHas(const TrackPathZone* z, float x, float y) {
	return x >= z->bbMin[0] && x <= z->bbMax[0] &&
	       y >= z->bbMin[1] && y <= z->bbMax[1];
}

// bIsPointInPoly (Speed.elf 0x8005DB24), instruction for instruction: the
// crossing-number test, with the edge taken as half-open in Y so a point
// exactly on a horizontal boundary belongs to one polygon and not both.
//
// The divide needs no guard: the branch above it is only taken when one of
// v[i].y, v[j].y is <= py and the other is strictly > py, so they differ.
static bool pointInPoly(float px, float py, const float* v, int n) {
	bool inside = false;
	int i, j = n - 1;
	for (i = 0; i < n; i++) {
		float viy = v[i * 2 + 1];
		float vjy = v[j * 2 + 1];
		if (((viy <= py) && (py < vjy)) || ((vjy <= py) && (py < viy))) {
			float vix = v[i * 2];
			float vjx = v[j * 2];
			if (px < (vjx - vix) * (py - viy) / (vjy - viy) + vix)
				inside = !inside;
		}
		j = i;
	}
	return inside;
}

TrackPaths* trackpath_open(const char* path, void (*log)(const char* msg)) {
	char line[160];

	FILE* f = fopen(path, "rb");
	if (!f) {
		// Expected on a card packed before this build: TRACKS/ROUTESL4RA is a
		// new copy step. Free roam keeps working, it just has no triggers.
		if (log) {
			snprintf(line, sizeof(line), "trackpath: no %s -- triggers off", path);
			log(line);
		}
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long fileSize = ftell(f);
	if (fileSize <= 16) {
		fclose(f);
		return NULL;
	}

	// The same flat {u32 type, u32 size} walk eventdb_load() does: top level
	// for the container, then its body for the zone chunk. Only the one
	// payload is ever read.
	long payload = -1;
	u32 payloadSize = 0;
	long off = 0;
	while (off + 8 <= fileSize) {
		u8 hdr[8];
		if (fseek(f, off, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8)
			break;
		u32 type = rd32le(hdr);
		u32 size = rd32le(hdr + 4);
		if (type == 0 && size == 0)
			break;
		if ((long)size > fileSize - (off + 8))
			break;
		if (type == TP_CONTAINER_TYPE) {
			long sub = off + 8;
			long end = sub + (long)size;
			while (sub + 8 <= end) {
				u8 sh[8];
				if (fseek(f, sub, SEEK_SET) != 0 || fread(sh, 1, 8, f) != 8)
					break;
				u32 st = rd32le(sh);
				u32 ss = rd32le(sh + 4);
				if (st == 0 && ss == 0)
					break;
				if ((long)ss > end - (sub + 8))
					break;
				if (st == TP_ZONE_CHUNK) {
					payload = sub + 8;
					payloadSize = ss;
					break;
				}
				sub += 8 + (long)ss;
			}
			break;
		}
		off += 8 + (long)size;
	}

	if (payload < 0 || payloadSize < TPZ_HEADER_BYTES) {
		fclose(f);
		if (log) {
			snprintf(line, sizeof(line),
				"trackpath: %s has no 0x3414A zone chunk", path);
			log(line);
		}
		return NULL;
	}

	TrackPaths* tp = (TrackPaths*)calloc(1, sizeof(TrackPaths));
	if (!tp) {
		fclose(f);
		return NULL;
	}
	// malloc is 8-aligned, and every stride is checked to be a multiple of 4
	// below, so every record below is 4-aligned and can be read in place.
	tp->blob = (u8*)malloc(payloadSize);
	if (!tp->blob) {
		free(tp);
		fclose(f);
		return NULL;
	}
	if (fseek(f, payload, SEEK_SET) != 0 ||
		fread(tp->blob, 1, payloadSize, f) != payloadSize) {
		fclose(f);
		trackpath_close(tp);
		return NULL;
	}
	fclose(f);
	tp->bytes = payloadSize;

	// Validate the whole walk before trusting any of it. A record that lies
	// about its stride would otherwise send the per-frame search off the end
	// of the buffer, and the file is the user's own copy of a retail asset --
	// not something to take on faith.
	u32 p = 0;
	int n = 0;
	u32 prevType = 0;
	while (p + TPZ_HEADER_BYTES <= tp->bytes) {
		const TrackPathZone* z = (const TrackPathZone*)(tp->blob + p);
		int stride = z->stride;
		int npts = z->numPoints;
		if (npts < 0 || stride <= 0 || (stride & 3) != 0 ||
			stride != TPZ_HEADER_BYTES + 8 * npts ||
			(u32)stride > tp->bytes - p ||
			z->type >= TPZ_TYPE_COUNT ||
			z->type < prevType ||
			n >= TP_MAX_ZONES) {
			if (log) {
				snprintf(line, sizeof(line),
					"trackpath: bad zone %d at +0x%lX (type %lu npts %d stride %d)",
					n, (unsigned long)p, (unsigned long)z->type, npts, stride);
				log(line);
			}
			trackpath_close(tp);
			return NULL;
		}
		prevType = z->type;
		tp->typeCount[z->type]++;
		p += (u32)stride;
		n++;
	}
	if (p != tp->bytes) {
		if (log) {
			snprintf(line, sizeof(line),
				"trackpath: %d zones end at +0x%lX, chunk is 0x%lX",
				n, (unsigned long)p, (unsigned long)tp->bytes);
			log(line);
		}
		trackpath_close(tp);
		return NULL;
	}
	tp->count = n;

	// FixupPointers (0x80201DC4): one forward pass, because the list is sorted
	// by type. A type with no zones gets begin == end and never matches.
	{
		const u8* cur = tp->blob;
		const u8* end = tp->blob + tp->bytes;
		u32 t;
		for (t = 0; t < TPZ_TYPE_COUNT; t++) {
			while (cur < end && ((const TrackPathZone*)cur)->type < t)
				cur += ((const TrackPathZone*)cur)->stride;
			tp->typeBegin[t] = cur;
		}
		tp->typeBegin[TPZ_TYPE_COUNT] = end;
	}

	if (log) {
		snprintf(line, sizeof(line),
			"trackpath: %d zones %lu KB -- %d race %d shop %d sms %d bank %d hood",
			tp->count, (unsigned long)(tp->bytes >> 10),
			tp->typeCount[TPZ_RACE_START_TRIGGER],
			tp->typeCount[TPZ_SHOP_TRIGGER],
			tp->typeCount[TPZ_SMS_TRIGGER],
			tp->typeCount[TPZ_BANK_TRIGGER],
			tp->typeCount[TPZ_NEIGHBOURHOOD]);
		log(line);
	}
	return tp;
}

void trackpath_close(TrackPaths* tp) {
	if (!tp)
		return;
	free(tp->blob);
	free(tp);
}

const TrackPathZone* trackpath_find(const TrackPaths* tp, float x, float y,
	u32 type, const TrackPathZone* after) {
	if (!tp || type >= TPZ_TYPE_COUNT)
		return NULL;
	const u8* end = tp->typeBegin[type + 1];
	const u8* p = after ? (const u8*)after + after->stride : tp->typeBegin[type];
	while (p < end) {
		const TrackPathZone* z = (const TrackPathZone*)p;
		if (boxHas(z, x, y) &&
			pointInPoly(x, y, trackpath_points(z), z->numPoints))
			return z;
		p += z->stride;
	}
	return NULL;
}

const TrackPathZone* trackpath_first(const TrackPaths* tp, u32 type,
	int* outCount) {
	if (outCount)
		*outCount = 0;
	if (!tp || type >= TPZ_TYPE_COUNT || tp->typeCount[type] == 0)
		return NULL;
	if (outCount)
		*outCount = tp->typeCount[type];
	return (const TrackPathZone*)tp->typeBegin[type];
}

const TrackPathZone* trackpath_next(const TrackPaths* tp,
	const TrackPathZone* z) {
	if (!tp || !z || z->type >= TPZ_TYPE_COUNT)
		return NULL;
	const u8* p = (const u8*)z + z->stride;
	return p < tp->typeBegin[z->type + 1] ? (const TrackPathZone*)p : NULL;
}

int trackpath_zone_count(const TrackPaths* tp) {
	return tp ? tp->count : 0;
}

int trackpath_type_count(const TrackPaths* tp, u32 type) {
	return (tp && type < TPZ_TYPE_COUNT) ? tp->typeCount[type] : 0;
}
