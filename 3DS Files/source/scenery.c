// Retail's scenery groups. See scenery.h for why, and tools/track_pack.py
// (SceneryGroups) for the format this reads and where it came from.
#include "scenery.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SCENERY_FILE     "WORLD.NFSG"
#define SCENERY_MAGIC    0x4753464e   // 'NFSG' little-endian
#define SCENERY_VERSION  1
#define SCENERY_HDR      16
#define SCENERY_NAME     32

// The instance flags a set's direction byte carries (track_pack.py).
#define INST_STARTFIN_FW  0x04   // hidden while the race runs backwards
#define INST_STARTFIN_BW  0x08   // hidden going forwards, and in free roam

// The most groups any race or career state turns on: two for the race, two
// for career free roam.
#define MAX_ON 4

typedef struct {
	u8  hidden;     // the instances carry 0x10 and need a group to show
	u8  direction;  // INST_STARTFIN_FW / _BW, 0 for neither
	u16 count;
	u32 first;      // into Scenery.members
} SetRec;

struct Scenery {
	u32     groupCount;
	char  (*names)[SCENERY_NAME];
	u32     setCount;
	SetRec* sets;
	u16*    members;
};

static void (*s_log)(const char*) = NULL;

void scenery_set_log(void (*sink)(const char* msg)) { s_log = sink; }

static void slog(const char* fmt, ...)
{
	if (!s_log)
		return;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	s_log(buf);
}

static u32 rd32(const u8* p, u32 off)
{
	return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) |
	       ((u32)p[off + 3] << 24);
}

static u16 rd16(const u8* p, u32 off)
{
	return (u16)(p[off] | (p[off + 1] << 8));
}

void scenery_close(Scenery* s)
{
	if (!s)
		return;
	free(s->names);
	free(s->sets);
	free(s->members);
	free(s);
}

Scenery* scenery_open(const char* dirPath)
{
	char path[400];
	snprintf(path, sizeof(path), "%s/%s", dirPath, SCENERY_FILE);
	FILE* f = fopen(path, "rb");
	if (!f) {
		slog("scenery: %s missing -- race barriers, start/finish lines and "
		     "shop beams stay hidden (run tools/track_pack.py)", SCENERY_FILE);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* b = (size >= SCENERY_HDR && size < (1 << 20)) ? (u8*)malloc((size_t)size) : NULL;
	if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) {
		free(b);
		fclose(f);
		slog("scenery: %s unreadable -- tagged scenery stays hidden", SCENERY_FILE);
		return NULL;
	}
	fclose(f);

	Scenery* s = NULL;
	const u32 groups = rd32(b, 8);
	const u32 sets = rd32(b, 12);
	if (rd32(b, 0) != SCENERY_MAGIC || rd32(b, 4) != SCENERY_VERSION ||
	    !sets || sets > SCENERY_MAX_SETS || groups > 0xFFFF ||
	    SCENERY_HDR + (size_t)groups * SCENERY_NAME > (size_t)size)
		goto bad;

	s = (Scenery*)calloc(1, sizeof(Scenery));
	if (!s)
		goto bad;
	s->groupCount = groups;
	s->setCount = sets;
	s->names = calloc(groups ? groups : 1, SCENERY_NAME);
	s->sets = (SetRec*)calloc(sets, sizeof(SetRec));
	// Each set's group list is at most what is left of the file, in u16s.
	s->members = (u16*)malloc((size_t)size);
	if (!s->names || !s->sets || !s->members)
		goto bad;
	for (u32 i = 0; i < groups; i++) {
		memcpy(s->names[i], b + SCENERY_HDR + i * SCENERY_NAME, SCENERY_NAME);
		s->names[i][SCENERY_NAME - 1] = '\0';
	}

	u32 o = SCENERY_HDR + groups * SCENERY_NAME;
	u32 used = 0;
	for (u32 i = 0; i < sets; i++) {
		if (o + 4 > (u32)size)
			goto bad;
		SetRec* r = &s->sets[i];
		r->hidden = b[o];
		r->direction = b[o + 1];
		r->count = rd16(b, o + 2);
		r->first = used;
		if (o + 4 + (u32)r->count * 2 > (u32)size)
			goto bad;
		for (u32 k = 0; k < r->count; k++) {
			const u16 g = rd16(b, o + 4 + k * 2);
			if (g >= groups)
				goto bad;
			s->members[used++] = g;
		}
		o += (4 + r->count * 2 + 3) & ~3u;
	}
	free(b);
	slog("scenery: %u groups, %u sets", (unsigned)groups, (unsigned)sets);
	return s;

bad:
	free(b);
	scenery_close(s);
	slog("scenery: %s is not a v%d NFSG -- tagged scenery stays hidden",
	     SCENERY_FILE, SCENERY_VERSION);
	return NULL;
}

// The global EnableGroup(name) (0x80196bd8), the scenery half. Like the
// topology half (topology.c, enableGroup), it goes through
// VisibleSectionManager::GetGroupInfo (0x8020bec4) first, which only knows
// names starting "BARRIERS_", "PLAYER_BARRIERS_" or "FREE_ROAM" (any case);
// then FindSceneryGroup looks the group up by name hash. The index of the
// group, or -1 when nothing was switched on.
static int enableGroup(const Scenery* s, const char* name)
{
	if (strncasecmp(name, "BARRIERS_", 9) != 0 &&
	    strncasecmp(name, "PLAYER_BARRIERS_", 16) != 0 &&
	    strncasecmp(name, "FREE_ROAM", 9) != 0)
		return -1;
	for (u32 i = 0; i < s->groupCount; i++)
		if (strcmp(s->names[i], name) == 0)
			return (int)i;
	return -1;
}

int scenery_set_groups(const Scenery* s, int trackId, int careerStage,
                       bool hasCareerCar, bool reverse, u8* on)
{
	memset(on, 0, SCENERY_SET_BYTES);
	if (!s)
		return 0;

	// InitTopologyAndSceneryGroups, in its order: the race's two barrier
	// sets, then -- with TheRaceParameters+0x14 at 0, and the game in career
	// -- the free-roam group and the stage's barriers. The free-roam group on
	// stage 0 is the one for whether the career has a car yet
	// (PlayerCareerState+0x7e2c): only the car lot's beams before, the
	// garage's as well after.
	char names[MAX_ON][SCENERY_NAME];
	int n = 0;
	snprintf(names[n++], SCENERY_NAME, "BARRIERS_%d", trackId);
	snprintf(names[n++], SCENERY_NAME, "PLAYER_BARRIERS_%d", trackId);
	if (careerStage >= 0) {
		const char* fr = "FREE_ROAM";
		if (careerStage == 0)
			fr = hasCareerCar ? "FREE_ROAM_STAGE0" : "FREE_ROAM_STAGE0A";
		snprintf(names[n++], SCENERY_NAME, "%s", fr);
		snprintf(names[n++], SCENERY_NAME, "BARRIERS_CAREER%d", careerStage);
	}
	int enabled[MAX_ON];
	int nOn = 0;
	char list[160];
	int len = 0;
	list[0] = '\0';
	for (int i = 0; i < n; i++) {
		const int g = enableGroup(s, names[i]);
		if (g < 0)
			continue;
		enabled[nOn++] = g;
		if (len < (int)sizeof(list) - 1)
			len += snprintf(list + len, sizeof(list) - len, " %s", names[i]);
	}

	// DrawAScenery's mask, the direction half: SetupSceneryCullInfo
	// (0x800c3c9c) puts 0x08 in it while TheRaceParameters+0x04 is 0 -- a race
	// run forwards, and free roam -- and 0x04 while it is 1.
	const u8 hideDir = reverse ? INST_STARTFIN_FW : INST_STARTFIN_BW;
	int drawn = 0;
	for (u32 i = 1; i < s->setCount; i++) {
		const SetRec* r = &s->sets[i];
		if (r->direction & hideDir)
			continue;
		bool show = !r->hidden;
		for (u32 k = 0; !show && k < r->count; k++)
			for (int j = 0; j < nOn; j++)
				if (s->members[r->first + k] == (u16)enabled[j]) {
					show = true;
					break;
				}
		if (show) {
			on[i >> 3] |= (u8)(1u << (i & 7));
			drawn++;
		}
	}
	slog("scenery: track %d%s, %d group(s) on:%s -- %d of %u sets drawn",
	     trackId, reverse ? " reversed" : "", nOn, nOn ? list : " none",
	     drawn, (unsigned)s->setCount - 1);
	return drawn;
}
