// Host driver for tools/custtune_check.py: the port's own cardb.c / perfdb.c /
// ridephys.c / custtune.c over GLOBALB.BUN, building each career car of a
// profile save the way RebuildPhysicsInfo (Speed.elf 0x801C89A8) does with its
// saved CustomPhysicsTunings (TASKS.md #417). Per car: the build without the
// tuning step (ridephys_build), the build with neutral all-100 bytes, and the
// build with each of the five saved settings that is not neutral, printing the
// CarPhysicsInfo floats that changed. The save is only read.
#include "cardb.h"
#include "perfdb.h"
#include "ridephys.h"
#include "custtune.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAYLOAD        0x24     // the save's header before the payload
#define OFF_CAREERCARS 0x5844   // FECareerCar[5]
#define STRIDE_CAREER  0x7F2
#define NUM_CAREER     5
#define REC_CARHASH    0x1C     // the slot array's car-type hash
#define REC_SPEC       0x2C8    // PresetCarSlot +0x2B4, PhysicsUpgradeSpecification
#define SPEC_INSTALLED 0x50
#define REC_TUNING     0x35C    // PresetCarSlot +0x348, CustomPhysicsTunings[5]

static const char* const kSetting[CUSTTUNE_NUM_SETTINGS] = {
	"Circuit", "Drag", "Drift", "Street X", "URL" };

// The CarPhysicsInfo floats the tuning types write (RebuildCustomTunings'
// SetRealValue targets), by offset.
static const char* field_name(int off) {
	static char b[32];
	switch (off) {
	case 0x018: return "wheel FL z";
	case 0x048: return "wheel FR z";
	case 0x078: return "wheel RL z";
	case 0x0A8: return "wheel RR z";
	case 0x0DC: return "front grip lat";
	case 0x0E0: return "front grip long";
	case 0x0FC: return "rear grip lat";
	case 0x100: return "rear grip long";
	case 0x174: return "front spring";
	case 0x178: return "front bump";
	case 0x17C: return "front rebound";
	case 0x180: return "front sway";
	case 0x194: return "rear spring";
	case 0x198: return "rear bump";
	case 0x19C: return "rear rebound";
	case 0x1A0: return "rear sway";
	case 0x1B8: return "front final drive";
	case 0x1BC: return "rear final drive";
	case 0x264: return "front downforce";
	case 0x268: return "rear downforce";
	case 0x270: return "steering";
	case 0x27C: return "brake bias";
	case 0x2A0: return "nitrous 0x2A0";
	case 0x2AC: return "nitrous 0x2AC";
	}
	if (off >= 0x1D8 && off <= 0x1EC) {
		snprintf(b, sizeof(b), "gear %d ratio", (off - 0x1D8) / 4 + 1);
		return b;
	}
	if (off >= 0x200 && off < 0x224) {
		snprintf(b, sizeof(b), "torque[%d]", (off - 0x200) / 4);
		return b;
	}
	if (off >= 0x224 && off < 0x248) {
		snprintf(b, sizeof(b), "boost[%d]", (off - 0x224) / 4);
		return b;
	}
	snprintf(b, sizeof(b), "?");
	return b;
}

static void diff(const RidePhys* a, const RidePhys* b) {
	int n = 0;
	for (int off = 0; off < RIDEPHYS_CPI_SIZE; off += 4) {
		float x, y;
		memcpy(&x, a->cpi + off, 4);
		memcpy(&y, b->cpi + off, 4);
		if (memcmp(&x, &y, 4) == 0)
			continue;
		printf("      +0x%03X %-18s %12.6g -> %12.6g\n", off, field_name(off), x, y);
		n++;
	}
	if (!n)
		printf("      (no CarPhysicsInfo float changed)\n");
	printf("      measure %.4f -> %.4f, top speed %.1f -> %.1f km/h\n", a->measure, b->measure,
		ridephys_top_speed(a) * 3.6f, ridephys_top_speed(b) * 3.6f);
}

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: custtune_check GLOBALB.BUN SAVE\n");
		return 2;
	}
	static CarDb db;
	static PerfDb pdb;
	if (!cardb_load(argv[1], &db) || !perfdb_load(argv[1], &pdb)) {
		fprintf(stderr, "cardb/perfdb load failed\n");
		return 1;
	}
	FILE* f = fopen(argv[2], "rb");
	if (!f) {
		fprintf(stderr, "cannot open the save\n");
		return 1;
	}
	static u8 save[0x20000];
	const size_t len = fread(save, 1, sizeof(save), f);
	fclose(f);
	const u8* pay = save + PAYLOAD;
	if (len < PAYLOAD + OFF_CAREERCARS + NUM_CAREER * STRIDE_CAREER) {
		fprintf(stderr, "the save is too short (%u bytes)\n", (unsigned)len);
		return 1;
	}
	u8* comp = malloc(RIDEPHYS_COMPLEAT_SIZE);
	static u8 neutral[CUSTTUNE_SIZE];
	memset(neutral, CUSTTUNE_DEFAULT_BYTE, sizeof(neutral));
	for (int i = 0; i < NUM_CAREER; i++) {
		const u8* rec = pay + OFF_CAREERCARS + i * STRIDE_CAREER;
		const u32 hash = rd32(rec + REC_CARHASH);
		const CarRec* r = NULL;
		for (int k = 0; k < db.count && !r; k++)
			if (db.recs[k].nameHash == hash)
				r = &db.recs[k];
		if (!r) {
			printf("career car %d: no car (hash %08X)\n", i, (unsigned)hash);
			continue;
		}
		if (!cardb_read_compleat(argv[1], &db, r->carType, comp)) {
			fprintf(stderr, "%s: cardb_read_compleat failed\n", r->name);
			return 1;
		}
		const u8* spec = rec + REC_SPEC;
		const u8* installed = spec + SPEC_INSTALLED;
		int junkman[RIDEPHYS_PATHS];
		for (int k = 0; k < RIDEPHYS_PATHS; k++)
			junkman[k] = (int)rd32(spec + 0x28 + k * 4);
		int fitted = 0;
		for (int k = 0; k < PERFDB_NUM_PARTS; k++)
			fitted += installed[k] != 0;

		static RidePhys plain, base, tuned;
		static CustTune ct;
		ridephys_build(&plain, comp, &pdb, installed, junkman);
		ridephys_build_spec(&base, &ct, comp, &pdb, installed, junkman, neutral);
		printf("career car %d: %s (type %d), %d part(s) fitted, %d tuning type(s) active:",
			i, r->name, r->carType, fitted, ct.numActive);
		for (int t = 0; t < CUSTTUNE_NUM_TYPES; t++)
			if (custtune_is_active(&ct, t))
				printf(" %d", t);
		printf("\n");
		printf("   no tuning step -> neutral (all-100) tunings:\n");
		diff(&plain, &base);
		for (int s = 0; s < CUSTTUNE_NUM_SETTINGS; s++) {
			const u8* tun = rec + REC_TUNING + s * CUSTTUNE_SIZE;
			if (custtune_tunings_equal(tun, neutral))
				continue;
			printf("   setting %d (%s):", s, kSetting[s]);
			for (int k = 0; k < CUSTTUNE_SIZE; k++)
				printf("%s%d", k ? "," : " ", tun[k]);
			printf("\n");
			ridephys_build_tuned(&tuned, comp, &pdb, installed, junkman, tun);
			diff(&base, &tuned);
		}
	}
	free(comp);
	return 0;
}
