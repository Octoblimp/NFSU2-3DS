// Host driver for tools/ridephys_check.py: loads GLOBALB.BUN through the port's
// own cardb.c / perfdb.c, builds every player car with ridephys.c stock and
// with every level-N package fitted, and prints the measure next to the one the
// game ships (row 0 = stock, the last row = fully upgraded) plus the three bar
// ratings (TASKS.md #116).
#include "cardb.h"
#include "perfdb.h"
#include "ridephys.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fit_level(const PerfDb* pdb, u8* installed, int level) {
	memset(installed, 0, PERFDB_NUM_PARTS);
	// Every package up to `level`, in order: a career car is fitted the same
	// way, one shop visit per level, and the upgrade value sums across levels.
	for (int l = PERFDB_LEVEL_MIN; l <= level; l++) {
		for (int t = 0; t < PERF_PKG_JUNKMAN; t++) {
			const PerformancePackage* p = perfdb_package(pdb, t, l);
			if (!p)
				continue;
			for (u32 k = 0; k < p->numParts && k < PERFDB_MAX_PARTS; k++)
				perfdb_install_part(pdb, installed, (int)p->part[k].partId, true, true);
		}
	}
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: ridephys_check GLOBALB.BUN\n");
		return 2;
	}
	static CarDb db;
	static PerfDb pdb;
	if (!cardb_load(argv[1], &db) || !db.extremaOk) {
		fprintf(stderr, "cardb_load failed\n");
		return 1;
	}
	if (!perfdb_load(argv[1], &pdb)) {
		fprintf(stderr, "perfdb_load failed\n");
		return 1;
	}
	u8* comp = malloc(RIDEPHYS_COMPLEAT_SIZE);
	static u8 rows[CARDB_PERF_MAX_ROWS * CARDB_PERF_ROW_SIZE];
	double worstStock = 0, worstMax = 0, worstRow = 0;
	int n = 0, nRows = 0, rowsOff = 0;
	for (int i = 0; i < db.count; i++) {
		const CarRec* r = &db.recs[i];
		if (r->kind != 0 || !r->measureValid)
			continue;
		if (!cardb_read_compleat(argv[1], &db, r->carType, comp)) {
			fprintf(stderr, "%s: cardb_read_compleat failed\n", r->name);
			return 1;
		}
		RidePhys rp;
		float rs[3], rl[4][3];
		ridephys_build(&rp, comp, NULL, NULL, NULL);
		const float mStock = rp.measure;
		ridephys_ratings(&rp, db.extrema, rs);
		float mLvl[4] = { mStock };
		memcpy(rl[0], rs, sizeof(rs));
		for (int l = 1; l <= 3; l++) {
			u8 inst[PERFDB_NUM_PARTS];
			fit_level(&pdb, inst, l);
			ridephys_build(&rp, comp, &pdb, inst, NULL);
			mLvl[l] = rp.measure;
			ridephys_ratings(&rp, db.extrema, rl[l]);
		}
		const double eS = fabs(mStock - r->measureStock) / r->measureStock;
		const double eM = fabs(mLvl[3] - r->measureMax) / r->measureMax;
		if (eS > worstStock) worstStock = eS;
		if (eM > worstMax) worstMax = eM;
		const bool cached = r->ratingValid &&
			r->rating[0] == rs[0] && r->rating[1] == rs[1] && r->rating[2] == rs[2];
		printf("%2d %-12s stock %8.3f/%8.3f  max %8.3f/%8.3f  A/T/H",
			r->carType, r->name, mStock, r->measureStock, mLvl[3], r->measureMax);
		for (int l = 0; l <= 3; l++)
			printf(" L%d %.3f %.3f %.3f", l, rl[l][0], rl[l][1], rl[l][2]);
		printf("%s\n", cached ? "" : "  CACHE-MISMATCH");

		// Every row of the car's table, through UnpackUpgradeInfo and the
		// build: the measure it ships is what RebuildPhysicsInfo gave those
		// parts (TASKS.md #288, the AI's MatchPerformance installs them).
		const int nr = cardb_read_perf_rows(argv[1], &db, r->carType, rows, CARDB_PERF_MAX_ROWS);
		double carWorst = 0;
		int carWorstRow = -1;
		for (int k = 0; k < nr; k++) {
			u8 inst[PERFDB_NUM_PARTS];
			float want;
			memcpy(&want, rows + k * CARDB_PERF_ROW_SIZE, sizeof(want));
			ridephys_unpack_row(rows + k * CARDB_PERF_ROW_SIZE, inst);
			ridephys_build(&rp, comp, &pdb, inst, NULL);
			const double e = fabs(rp.measure - want) / want;
			if (e > carWorst) { carWorst = e; carWorstRow = k; }
			if (e > 1e-4) rowsOff++;
			nRows++;
		}
		if (carWorst > worstRow) worstRow = carWorst;
		printf("   %d rows, worst %.3g (row %d)\n", nr, carWorst, carWorstRow);

		// MatchPerformance against a few targets across the car's range, with
		// and without the handling pass.
		const float lo = r->measureStock, hi = r->measureMax;
		for (int t = 0; t <= 4; t++) {
			const float target = lo + (hi - lo) * (float)t / 4.0f + 3.0f;
			u8 inst[PERFDB_NUM_PARTS];
			memset(inst, 0, sizeof(inst));
			ridephys_match_performance(&rp, inst, comp, &pdb, rows, nr, target, -1.0f, NULL);
			const float plain = rp.measure;
			ridephys_match_performance(&rp, inst, comp, &pdb, rows, nr, target, 0.6f, db.extrema);
			printf("   match %.2f -> %.2f; handling 0.6 -> %.2f (handling %.3f)\n", target,
				plain, rp.measure, ridephys_rating(&rp, db.extrema, RIDEPHYS_RATING_HANDLING));
		}
		n++;
	}
	printf("%d rows across the cars; %d off by more than 1e-4, worst %.3g\n", nRows, rowsOff,
		worstRow);
	printf("%d cars; worst stock measure error %.3g, worst max %.3g\n", n, worstStock, worstMax);
	free(comp);
	return 0;
}
