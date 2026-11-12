// See lightmat.h. The file layout is documented at the top of
// tools/lightmat_pack.py.
#include "lightmat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFSL_MAGIC   0x4C53464Eu  // 'NFSL' little-endian
#define NFSL_VERSION 1
#define NFSL_HDR     0x10
#define NFSL_ENTRY   144
#define NFSL_NAME    28
#define LIGHTMAT_MAX 255

static LightMat* s_mats;
static int s_count;
static bool s_tried;

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static float rdf(const u8* p) {
	const u32 v = rd32(p);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

// The (int) of a float, held to a byte: UpdatePlatInfo's fctiwz and clamp.
static u8 sat_byte(float v) {
	const int i = (int)v;
	return (u8)(i < 0 ? 0 : i > 255 ? 255 : i);
}

// UpdatePlatInfo (Speed.elf 0x801e2a64) overrides MinA and MaxA for the
// glass, by the material's name: values out of its tables at 0x8036059c /
// 0x803605ac and r13-0x64ac / -0x64a8 (the tweakables HeadlightGlass-
// DiffuseMinA/MaxA and their like).
static bool glass_alpha(u32 hash, float* minA, float* maxA) {
	switch (hash) {
	case 0x3ed70c43u:  // WINDOWMASK
	case 0x12c9453cu:  // MOLDINGS
	case 0x471a1dcau:  // WINDSHIELD
		*minA = 0.3f; *maxA = 1.0f; return true;
	case 0x89f23a14u:  // WINDSHIELD_TINT_L1_GREEN
	case 0x89946400u:  // WINDSHIELD_TINT_L1_BLACK
		*minA = 0.3f; *maxA = 1.0f; return true;
	case 0x8050ca3fu:  // WINDSHIELD_TINT_L2_RED
	case 0x8a616c4cu:  // WINDSHIELD_TINT_L2_BLUE
	case 0xd68ea0c1u:  // WINDSHIELD_TINT_L2_BLACK
	case 0xd6ec76d5u:  // WINDSHIELD_TINT_L2_GREEN
		*minA = 0.4f; *maxA = 1.0f; return true;
	case 0x23e6a3f4u:  // WINDSHIELD_TINT_L3_GRAPE
	case 0x2388dd82u:  // WINDSHIELD_TINT_L3_BLACK
	case 0x23e6b396u:  // WINDSHIELD_TINT_L3_GREEN
	case 0x6cf41096u:  // WINDSHIELD_TINT_L3_LAVENDER
	case 0x8062e2c0u:  // WINDSHIELD_TINT_L3_RED
	case 0x8cbe3497u:  // WINDSHIELD_TINT_L3_PINK
	case 0x8cb694edu:  // WINDSHIELD_TINT_L3_BLUE
	case 0x8cb61dcdu:  // WINDSHIELD_TINT_L3_AQUA
	case 0x8cb95e6bu:  // WINDSHIELD_TINT_L3_GOLD
	case 0x8cbc02ecu:  // WINDSHIELD_TINT_L3_LIME
	case 0xc9d2b801u:  // WINDSHIELD_TINT_L3_YELLOW
	case 0xb3645f81u:  // WINDSHIELD_TINT_L3_ORANGE
		*minA = 0.5f; *maxA = 1.0f; return true;
	case 0x33a26cb6u:  // CLEARPLASTIC
	case 0xa6348ee3u:  // HEADLIGHTGLASS
		*minA = 0.4f; *maxA = 0.8f; return true;
	default:
		return false;
	}
}

// UpdatePlatInfo, from the record's 28 floats (lightmat_pack.py names them).
static void derive(LightMat* m, const u8* v) {
	float f[28];
	for (int i = 0; i < 28; i++)
		f[i] = rdf(v + i * 4);
	for (int c = 0; c < 3; c++) {
		const float dmin = f[0] * f[1 + c];
		const float smin = f[11] * f[12 + c];
		m->diffMin[c] = dmin;
		m->diffRange[c] = f[4] * f[5 + c] - dmin;
		m->specMin[c] = smin;
		m->specRange[c] = f[15] * f[16 + c] - smin;
		m->k0[c] = sat_byte(f[20] * f[21 + c] * 128.0f);
		m->k1[c] = sat_byte(f[24] * f[25 + c] * 128.0f);
	}
	float minA = f[8], maxA = f[9];
	glass_alpha(m->hash, &minA, &maxA);
	m->minA = minA;
	m->rangeA = maxA - minA;
	m->specPower = f[10];
	m->envPower = f[19];
}

bool lightmat_load(const char* dataRoot) {
	if (s_tried)
		return s_mats != NULL;
	s_tried = true;
	char path[512];
	snprintf(path, sizeof(path), "%s/GLOBAL/LIGHTMATS.nfsl", dataRoot);
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* b = size >= NFSL_HDR ? (u8*)malloc((size_t)size) : NULL;
	bool ok = b && fread(b, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	u32 n = 0, eo = 0;
	if (ok) {
		n = rd32(b + 8);
		eo = rd32(b + 12);
		ok = rd32(b) == NFSL_MAGIC && rd32(b + 4) == NFSL_VERSION && n > 0 &&
		     n <= LIGHTMAT_MAX && (u64)eo + (u64)n * NFSL_ENTRY <= (u64)size;
	}
	LightMat* mats = ok ? (LightMat*)calloc(n, sizeof(LightMat)) : NULL;
	if (mats) {
		for (u32 i = 0; i < n; i++) {
			const u8* e = b + eo + i * NFSL_ENTRY;
			LightMat* m = &mats[i];
			m->hash = rd32(e);
			memcpy(m->name, e + 4, NFSL_NAME);
			m->name[NFSL_NAME - 1] = '\0';
			derive(m, e + 4 + NFSL_NAME);
		}
		s_mats = mats;
		s_count = (int)n;
	}
	free(b);
	return s_mats != NULL;
}

const LightMat* lightmat_find(u32 hash) {
	int lo = 0, hi = s_count;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		if (s_mats[mid].hash == hash)
			return &s_mats[mid];
		if (s_mats[mid].hash < hash) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

const LightMat* lightmat_get(u32 hash) {
	const LightMat* m = lightmat_find(hash);
	return m ? m : lightmat_find(LIGHTMAT_DEFAULT);
}

u8 lightmat_index(const LightMat* m) {
	return m ? (u8)(m - s_mats) : 0xff;
}

const LightMat* lightmat_at(u8 index) {
	return index < s_count ? &s_mats[index] : NULL;
}
