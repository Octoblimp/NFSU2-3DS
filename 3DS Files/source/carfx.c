// The frontend car's Nitrous Purge spray; see carfx.h.
#include "carfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PURGE_GROUP_MAX     4
#define PURGE_GROUP_SIZE    (8 + PURGE_GROUP_MAX * 40)
#define PURGE_PART_SIZE     (16 + 32 * 4)
// CarRenderInfo's position lists by emitter posType (template +0x5c).
#define POS_HOOD            28
#define POS_WHEEL           29
// The exhaust list (CarRenderInfo+0x58C) and its effect's group:
// bStringHash("CARFX_NITRO"), CarEffectParameters' entry 10.
#define POS_EXHAUST         10
#define GROUP_NITRO         0x0027391Eu
#define MAX_NITRO           PURGE_GROUP_MAX
// bStringHash("XECH_FLAG_GIRL_FLARE"), FlagGirlFlareEffect's group (#497).
#define GROUP_FLARE         0xE1370B16u
#define MAX_FLARE           PURGE_GROUP_MAX
// TriggerEffect's constants (Speed.elf 0x8004F1CC): the particles leave the
// flare at twice its unit axis (DAT_803195d4 0 for the fourth row), and
// TriggerEmitter steps -1.0 (DAT_803195d8), one burst a call.
#define FLARE_SPEED         2.0f
#define FLARE_INTENSITY     1.0f
// HOODLEFT, HOODRIGHT, WHEELLEFT, WHEELRIGHT as carfx_pack.py packs them.
#define F_HOOD_L            1
#define F_HOOD_R            2
#define F_WHEEL_L           4
#define F_WHEEL_R           8
// The CARFX_BREAK_* groups (#513), CarEffectParameters 15..29 by index:
// bStringHash of the name. GLOBALB.BUN has emitters for 19..22 and 24..29
// (XECD_*_Glass, XECD_WindShieldA, XECD_WindowRearA); tools/carfx_pack.py
// packs those.
#define BREAK_FIRST         15
#define BREAK_EFFECTS       15
#define MAX_BREAK           2
static const u32 kBreakGroup[BREAK_EFFECTS] = {
	0x2B1F953Fu, 0x8F8104B2u, 0x2643EF16u, 0x271802EBu,   // mirrors, plates
	0xACDF6E4Au, 0x493BFF1Du, 0x7D50F61Du, 0x27DE8150u,   // head, brake L/R
	0x006CF193u,                                          // brake centre
	0x19193E41u, 0x2CEA92F7u, 0xB23A03E0u, 0x72081D81u,   // windshield, rear, LF, LR
	0x75F058B3u, 0xD50DA3F4u,                             // RF, RR
};
// A purge part names one group per list; the C family has two members each.
#define MAX_COPIES          (2 * PURGE_GROUP_MAX)
// FrontEndRenderingCar::TriggerEffect's intensity (DAT_8033baf4).
#define FE_INTENSITY        1.0f

typedef struct {
	u16   tmpl;
	u16   posType;
	float rot[9];
} PurgeMember;

typedef struct {
	u32         hash;
	int         count;
	PurgeMember m[PURGE_GROUP_MAX];
} PurgeGroup;

typedef struct {
	u32   hash;
	u32   hoodGroup, wheelGroup;
	u8    flags;
	float hood[16];
	float tire[16];
} PurgePart;

// One nozzle: a CarEmitterPosition, its flip bits split out.
typedef struct {
	float pos[3];
	bool  flipY;
} Nozzle;

typedef struct {
	s16          h;
	u16          posType;
	float        rot[9];
	const float* color;
} Copy;

static void (*s_log)(const char*) = NULL;
static Afx* s_afx;
static PurgeGroup* s_groups;
static int s_nGroups;
static PurgePart* s_parts;
static int s_nParts;
static u32 s_part;           // the part the lists were built for
static bool s_partSet;
static Copy s_copy[MAX_COPIES];
static int s_nCopy;
static Nozzle s_hood[2], s_wheel[2];
static int s_nHood, s_nWheel;
// The last carfx_set_purge() request, kept across carfx_close() as the
// exhaust tips are (#478): the pack closes and opens again around the world
// and the in-world shops, and a car whose parts were set while it was shut,
// or that is not rebuilt after an open, would otherwise lose its purge -- Y
// then only sounds. carfx_open() puts it back.
static struct {
	bool have;
	u32 part;
	float hood[2][3];
	bool hoodOk[2];
	bool wheelsOk;
	float wheel[2][4];
} s_want;
static void applyPurge(void);
// The nitrous flame's copies (made on its first trigger after an open) and
// the exhaust tips, which outlive the pack.
static Copy s_nitro[MAX_NITRO];
static int s_nNitro;
static bool s_nitroTried;
static float s_exhaust[CARKIT_EXHAUSTS][3];
// The flag girl's flare emitters (#497): one set, as retail's one
// FlagGirlFlareEffect, moved to each flare in turn.
static Copy s_flare[MAX_FLARE];
static int s_nFlare;
static bool s_flareTried;
static int s_nExhaust;
// The glass (#513): each break effect's copies, made on its first trigger
// after an open, and the position lists, which outlive the pack.
static Copy s_break[BREAK_EFFECTS][MAX_BREAK];
static u8 s_nBreak[BREAK_EFFECTS];
static bool s_breakTried[BREAK_EFFECTS];
static float s_glass[CARKIT_GLASS_LISTS][CARKIT_GLASS_POS][3];
static u8 s_glassN[CARKIT_GLASS_LISTS];

void carfx_set_log(void (*sink)(const char* msg)) { s_log = sink; }

static void clog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void clog(const char* fmt, ...) {
	if (!s_log)
		return;
	char buf[192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	s_log(buf);
}

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }

static float rdf(const u8* p) {
	u32 v = rd32(p);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

// The purge table, from the trailer at the end of the file.
static bool readPurge(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	u8 tail[8];
	bool ok = fseek(f, 0, SEEK_END) == 0;
	const long size = ok ? ftell(f) : -1;
	ok = size >= 16 && fseek(f, size - 8, SEEK_SET) == 0 && fread(tail, 1, 8, f) == 8 &&
	     memcmp(tail + 4, "PRGT", 4) == 0;
	const u32 off = ok ? rd32(tail) : 0;
	ok = ok && off + 8 <= (u32)size - 8;
	const u32 len = ok ? (u32)size - 8 - off : 0;
	u8* t = ok ? (u8*)malloc(len) : NULL;
	ok = t && fseek(f, (long)off, SEEK_SET) == 0 && fread(t, 1, len, f) == len &&
	     memcmp(t, "NFSP", 4) == 0;
	fclose(f);
	if (!ok) {
		free(t);
		return false;
	}
	const int ng = rd16(t + 4), np = rd16(t + 6);
	ok = 8u + (u32)ng * PURGE_GROUP_SIZE + (u32)np * PURGE_PART_SIZE <= len;
	s_groups = ok ? (PurgeGroup*)calloc(ng ? ng : 1, sizeof(PurgeGroup)) : NULL;
	s_parts = ok ? (PurgePart*)calloc(np ? np : 1, sizeof(PurgePart)) : NULL;
	if (!s_groups || !s_parts) {
		free(t);
		return false;
	}
	const int nTmpl = afx_tmpl_count(s_afx);
	const u8* p = t + 8;
	for (int g = 0; g < ng; g++, p += PURGE_GROUP_SIZE) {
		PurgeGroup* G = &s_groups[g];
		G->hash = rd32(p);
		const int n = p[4] > PURGE_GROUP_MAX ? PURGE_GROUP_MAX : p[4];
		for (int k = 0; k < n; k++) {
			const u8* m = p + 8 + k * 40;
			const u16 ti = rd16(m);
			if (ti >= nTmpl)
				continue;
			PurgeMember* M = &G->m[G->count++];
			M->tmpl = ti;
			M->posType = rd16(m + 2);
			for (int i = 0; i < 9; i++)
				M->rot[i] = rdf(m + 4 + i * 4);
		}
	}
	for (int i = 0; i < np; i++, p += PURGE_PART_SIZE) {
		PurgePart* P = &s_parts[i];
		P->hash = rd32(p);
		P->hoodGroup = rd32(p + 4);
		P->wheelGroup = rd32(p + 8);
		P->flags = p[12];
		for (int k = 0; k < 16; k++) {
			P->hood[k] = rdf(p + 16 + k * 4);
			P->tire[k] = rdf(p + 80 + k * 4);
		}
	}
	s_nGroups = ng;
	s_nParts = np;
	free(t);
	return true;
}

bool carfx_open(const char* path) {
	carfx_close();
	afx_set_log(s_log);
	s_afx = afx_open(path);
	if (!s_afx) {
		clog("carfx: no %s -- the purge only sounds", path);
		return false;
	}
	if (!readPurge(path)) {
		clog("carfx: %s has no purge table -- the purge only sounds", path);
		carfx_close();
		return false;
	}
	clog("carfx: %d purge parts, %d emitter groups", s_nParts, s_nGroups);
	if (s_want.have)
		applyPurge();
	return true;
}

void carfx_close(void) {
	afx_close(s_afx);
	s_afx = NULL;
	free(s_groups);
	free(s_parts);
	s_groups = NULL;
	s_parts = NULL;
	s_nGroups = s_nParts = 0;
	s_nCopy = 0;
	s_nNitro = 0;
	s_nitroTried = false;
	s_nFlare = 0;
	s_flareTried = false;
	memset(s_nBreak, 0, sizeof(s_nBreak));
	memset(s_breakTried, 0, sizeof(s_breakTried));
	s_partSet = false;
	s_part = 0;
}

bool carfx_is_open(void) { return s_afx != NULL; }

static const PurgePart* findPart(u32 h) {
	int lo = 0, hi = s_nParts;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		if (s_parts[mid].hash == h)
			return &s_parts[mid];
		if (s_parts[mid].hash < h) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

static const PurgeGroup* findGroup(u32 h) {
	for (int i = 0; h && i < s_nGroups; i++)
		if (s_groups[i].hash == h)
			return &s_groups[i];
	return NULL;
}

// afxGetEmitters for one list: a state-3 copy of every member, coloured with
// the part's override (NosBlowoffOverrideColours, afxShiftEmitterColours).
static void addCopies(const PurgeGroup* g, const float* color) {
	for (int k = 0; g && k < g->count && s_nCopy < MAX_COPIES; k++) {
		const s16 h = afx_car_emitter(s_afx, g->m[k].tmpl);
		if (h < 0) {
			clog("carfx: no free emitter for group %08lx", (unsigned long)g->hash);
			return;
		}
		Copy* c = &s_copy[s_nCopy++];
		c->h = h;
		c->posType = g->m[k].posType;
		memcpy(c->rot, g->m[k].rot, sizeof(c->rot));
		c->color = color;
	}
}

void carfx_set_purge(u32 part, const CarKitInfo* kit, const float wheel[2][4]) {
	if (s_afx && s_partSet && part == s_part)
		return;
	s_want.have = true;
	s_want.part = part;
	for (int i = 0; i < 2; i++) {
		s_want.hoodOk[i] = kit && kit->purgeHoodOk[i];
		if (s_want.hoodOk[i])
			memcpy(s_want.hood[i], kit->purgeHood[i], sizeof(s_want.hood[i]));
	}
	s_want.wheelsOk = wheel != NULL;
	if (wheel)
		memcpy(s_want.wheel, wheel, sizeof(s_want.wheel));
	if (s_afx)
		applyPurge();
	else
		clog("carfx: purge part %08lx kept for the next open", (unsigned long)part);
}

static void applyPurge(void) {
	const u32 part = s_want.part;
	// The lists and positions go (their particles live out their time).
	for (int i = 0; i < s_nCopy; i++)
		afx_detach(s_afx, s_copy[i].h);
	s_nCopy = s_nHood = s_nWheel = 0;
	s_part = part;
	s_partSet = true;
	const PurgePart* p = part ? findPart(part) : NULL;
	if (!p) {
		if (part)
			clog("carfx: purge part %08lx is not in the table", (unsigned long)part);
		return;
	}
	addCopies(findGroup(p->hoodGroup), p->hood);
	addCopies(findGroup(p->wheelGroup), p->tire);
	// HOODLEFT on marker 0xE3DE69DB as it is, HOODRIGHT on 0x601A6CCE
	// mirrored across y; WHEELLEFT at the front-left wheel's leading edge
	// mirrored, WHEELRIGHT at the front-right's as it is.
	if ((p->flags & F_HOOD_L) && s_want.hoodOk[0]) {
		memcpy(s_hood[s_nHood].pos, s_want.hood[0], sizeof(s_hood[0].pos));
		s_hood[s_nHood++].flipY = false;
	}
	if ((p->flags & F_HOOD_R) && s_want.hoodOk[1]) {
		memcpy(s_hood[s_nHood].pos, s_want.hood[1], sizeof(s_hood[0].pos));
		s_hood[s_nHood++].flipY = true;
	}
	for (int w = 0; w < 2 && s_want.wheelsOk; w++) {
		if (!(p->flags & (w ? F_WHEEL_R : F_WHEEL_L)))
			continue;
		const float* wh = s_want.wheel[w];
		Nozzle* n = &s_wheel[s_nWheel++];
		n->pos[0] = wh[0] + wh[3];
		n->pos[1] = wh[1];
		n->pos[2] = wh[2];
		n->flipY = w == 0;
	}
	clog("carfx: purge part %08lx -- %d emitters, %d hood and %d wheel nozzles (flags %x)",
		(unsigned long)part, s_nCopy, s_nHood, s_nWheel, p->flags);
	for (int i = 0; i < s_nHood; i++)
		clog("carfx:   hood %d at %.2f %.2f %.2f%s", i, s_hood[i].pos[0], s_hood[i].pos[1],
			s_hood[i].pos[2], s_hood[i].flipY ? " (mirrored)" : "");
	for (int i = 0; i < s_nWheel; i++)
		clog("carfx:   wheel %d at %.2f %.2f %.2f%s", i, s_wheel[i].pos[0], s_wheel[i].pos[1],
			s_wheel[i].pos[2], s_wheel[i].flipY ? " (mirrored)" : "");
}

// CarRenderInfo::TriggerEffect's matrix for one copy at one position: the
// copy's rows (y column negated when mirrored) times the car's frame, the
// position carried through it to the world.
static void emitterMatrix(float m[16], const float rot9[9], bool flipY, const float p[3],
		const float rot[3][3], const float origin[3]) {
	float local[9];
	memcpy(local, rot9, sizeof(local));
	if (flipY)
		local[1] = -local[1], local[4] = -local[4], local[7] = -local[7];
	for (int r = 0; r < 3; r++) {
		for (int k = 0; k < 3; k++)
			m[r * 4 + k] = local[r * 3] * rot[0][k] + local[r * 3 + 1] * rot[1][k] +
			               local[r * 3 + 2] * rot[2][k];
		m[r * 4 + 3] = 0.0f;
	}
	for (int k = 0; k < 3; k++)
		m[12 + k] = p[0] * rot[0][k] + p[1] * rot[1][k] + p[2] * rot[2][k] + origin[k];
	m[15] = 1.0f;
}

void carfx_set_exhaust(const CarKitInfo* kit) {
	s_nExhaust = kit ? kit->exhausts : 0;
	if (s_nExhaust > CARKIT_EXHAUSTS)
		s_nExhaust = CARKIT_EXHAUSTS;
	for (int i = 0; i < s_nExhaust; i++)
		memcpy(s_exhaust[i], kit->exhaust[i], sizeof(s_exhaust[i]));
	clog("carfx: %d exhaust tips for the nitrous flame", s_nExhaust);
	for (int i = 0; i < s_nExhaust; i++)
		clog("carfx:   exhaust %d at %.2f %.2f %.2f", i, s_exhaust[i][0], s_exhaust[i][1],
			s_exhaust[i][2]);
}

int carfx_exhausts(void) { return s_nExhaust; }

// Debug (#317): a few of the flame's particles in the car's frame (x forward,
// y left, z up from `origin`), with their age, colour and size.
void carfx_probe_nitro(const float rot[3][3], const float origin[3]) {
	if (!s_afx || !s_nNitro)
		return;
	float pos[6][3], tl[6], col[6][4], size[6];
	const int n = afx_probe(s_afx, s_nitro[0].h, 6, pos, tl, col, size);
	clog("carfx:   the last draw put up %d quads", afx_last_quads(s_afx));
	for (int i = 0; i < n; i++) {
		float d[3], l[3];
		for (int k = 0; k < 3; k++)
			d[k] = pos[i][k] - origin[k];
		for (int r = 0; r < 3; r++)
			l[r] = d[0] * rot[r][0] + d[1] * rot[r][1] + d[2] * rot[r][2];
		clog("carfx:   flame %d at %.2f %.2f %.2f (car) age %.2f rgba %.0f %.0f %.0f %.0f size %.2f",
			i, l[0], l[1], l[2], tl[i], col[i][0], col[i][1], col[i][2], col[i][3], size[i]);
	}
}

void carfx_car_velocity(const float vel[3]) {
	for (int i = 0; s_afx && i < s_nNitro; i++)
		afx_attach_move(s_afx, s_nitro[i].h, NULL, vel);
	for (int e = 0; s_afx && e < BREAK_EFFECTS; e++)
		for (int i = 0; i < s_nBreak[e]; i++)
			afx_attach_move(s_afx, s_break[e][i].h, NULL, vel);
}

int carfx_set_glass(const CarKitInfo* kit) {
	int n = 0;
	for (int l = 0; l < CARKIT_GLASS_LISTS; l++) {
		s_glassN[l] = kit ? kit->glassPosN[l] : 0;
		if (s_glassN[l] > CARKIT_GLASS_POS)
			s_glassN[l] = CARKIT_GLASS_POS;
		if (s_glassN[l])
			memcpy(s_glass[l], kit->glassPos[l], s_glassN[l] * sizeof(s_glass[l][0]));
		n += s_glassN[l];
	}
	return n;
}

// The emitter's posType to CARKIT_GLASS_LISTS' index, -1 for none.
static int glassList(u16 posType) {
	if (posType >= 12 && posType <= 17)
		return posType - 12;
	if (posType >= 20 && posType <= 23)
		return posType - 14;
	return -1;
}

void carfx_trigger_break(int effect, float intensity, const float rot[3][3],
		const float origin[3], const float vel[3], const float eye[3]) {
	const int e = effect - BREAK_FIRST;
	if (!s_afx || e < 0 || e >= BREAK_EFFECTS || !(intensity > 0.0f))
		return;
	if (!s_breakTried[e]) {
		// afxGetEmitters(list effect, CARFX_BREAK_*): a copy of every member.
		s_breakTried[e] = true;
		const PurgeGroup* g = findGroup(kBreakGroup[e]);
		for (int k = 0; g && k < g->count && s_nBreak[e] < MAX_BREAK; k++) {
			const s16 h = afx_car_emitter(s_afx, g->m[k].tmpl);
			if (h < 0)
				break;
			Copy* c = &s_break[e][s_nBreak[e]++];
			c->h = h;
			c->posType = g->m[k].posType;
			memcpy(c->rot, g->m[k].rot, sizeof(c->rot));
			c->color = NULL;
		}
	}
	int bursts = 0;
	for (int i = 0; i < s_nBreak[e]; i++) {
		const Copy* c = &s_break[e][i];
		const int l = glassList(c->posType);
		for (int j = 0; l >= 0 && j < s_glassN[l]; j++) {
			float m[16];
			emitterMatrix(m, c->rot, false, s_glass[l][j], rot, origin);
			afx_burst(s_afx, c->h, m, vel, intensity, c->color, eye);
			bursts++;
		}
	}
	clog("carfx: break effect %d at %.1f -- %d emitters, %d bursts, %d particles alive", effect,
		(double)intensity, s_nBreak[e], bursts, carfx_particles());
}

void carfx_trigger_nitro(float intensity, const float rot[3][3], const float origin[3],
		const float vel[3], const float eye[3]) {
	if (!s_afx || !s_nExhaust || !(intensity > 0.0f))
		return;
	if (!s_nitroTried) {
		// afxGetEmitters(list 10, CARFX_NITRO): a copy of every member.
		s_nitroTried = true;
		const PurgeGroup* g = findGroup(GROUP_NITRO);
		for (int k = 0; g && k < g->count && s_nNitro < MAX_NITRO; k++) {
			const s16 h = afx_car_emitter(s_afx, g->m[k].tmpl);
			if (h < 0)
				break;
			Copy* c = &s_nitro[s_nNitro++];
			c->h = h;
			c->posType = g->m[k].posType;
			memcpy(c->rot, g->m[k].rot, sizeof(c->rot));
			c->color = NULL;
		}
		clog("carfx: nitrous flame -- %d emitters%s", s_nNitro,
			g ? "" : " (CARFX.nfsa packed before #317: repack it)");
	}
	for (int i = 0; i < s_nNitro; i++) {
		const Copy* c = &s_nitro[i];
		if (c->posType != POS_EXHAUST)
			continue;
		for (int j = 0; j < s_nExhaust; j++) {
			float m[16];
			emitterMatrix(m, c->rot, false, s_exhaust[j], rot, origin);
			afx_burst(s_afx, c->h, m, vel, intensity, c->color, eye);
		}
	}
}

// FlagGirlFlareEffect::TriggerEffect's emitter half (Speed.elf 0x8004F1CC):
// every member at an identity matrix on `pos`, inheriting the flare's unit
// axis times 2, TriggerEmitter(-1.0).
void carfx_trigger_flare(const float pos[3], const float dir[3], const float eye[3]) {
	if (!s_afx)
		return;
	if (!s_flareTried) {
		s_flareTried = true;
		const PurgeGroup* g = findGroup(GROUP_FLARE);
		for (int k = 0; g && k < g->count && s_nFlare < MAX_FLARE; k++) {
			const s16 h = afx_car_emitter(s_afx, g->m[k].tmpl);
			if (h < 0)
				break;
			Copy* c = &s_flare[s_nFlare++];
			c->h = h;
			c->posType = g->m[k].posType;
			memcpy(c->rot, g->m[k].rot, sizeof(c->rot));
			c->color = NULL;
		}
		clog("carfx: the flag girl's flares -- %d emitters%s", s_nFlare,
			g ? "" : " (CARFX.nfsa packed before #497: repack it)");
	}
	if (!s_nFlare)
		return;
	const float l = __builtin_sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	const float k = l > 0.0f ? FLARE_SPEED / l : 0.0f;
	const float vel[3] = { dir[0] * k, dir[1] * k, dir[2] * k };
	const float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, pos[0], pos[1], pos[2], 1 };
	for (int i = 0; i < s_nFlare; i++)
		afx_burst(s_afx, s_flare[i].h, m, vel, FLARE_INTENSITY, NULL, eye);
}

void carfx_trigger_purge(const float rot[3][3], const float origin[3], const float eye[3]) {
	if (!s_afx || !s_nCopy)
		return;
	static const float still[3] = { 0.0f, 0.0f, 0.0f };
	for (int i = 0; i < s_nCopy; i++) {
		const Copy* c = &s_copy[i];
		const Nozzle* list = c->posType == POS_HOOD ? s_hood : c->posType == POS_WHEEL ? s_wheel : NULL;
		const int n = c->posType == POS_HOOD ? s_nHood : c->posType == POS_WHEEL ? s_nWheel : 0;
		for (int j = 0; list && j < n; j++) {
			float m[16];
			emitterMatrix(m, c->rot, list[j].flipY, list[j].pos, rot, origin);
			afx_burst(s_afx, c->h, m, still, FE_INTENSITY, c->color, eye);
		}
	}
}

void carfx_update(float dt, const float eye[3]) {
	if (s_afx)
		afx_update(s_afx, dt, eye);
}

void carfx_draw(const C3D_Mtx* vp, const AfxView* view) {
	if (s_afx)
		afx_draw(s_afx, vp, view);
}

void carfx_purge_state(u32* part, int* copies, int* nozzles, int* quads) {
	if (part)
		*part = s_afx && s_partSet ? s_part : 0;
	if (copies)
		*copies = s_afx ? s_nCopy : 0;
	if (nozzles)
		*nozzles = s_afx ? s_nHood + s_nWheel : 0;
	if (quads)
		*quads = s_afx ? afx_last_quads(s_afx) : -2;
}

int carfx_particles(void) {
	int n = 0;
	if (s_afx)
		afx_stats(s_afx, NULL, &n);
	return n;
}
