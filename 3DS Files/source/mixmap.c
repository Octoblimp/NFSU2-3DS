// NFSU2-3DS: retail's mix map, over the PC MAPOUTPUT.mxb. See mixmap.h.
//
// Struct fields carry the retail offsets they stand for. The file is the PC
// one, little-endian like the 3DS, so its records are read in place; where the
// GameCube code reads a halfword out of a big-endian word, the port takes the
// matching half (noted where it happens).
#include "mixmap.h"

#include "audio.h"
#include "cardb.h"
#include "dbglog.h"
#include "mixmap_tables.h"
#include "speech.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mixLog(const char* fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	dbglog_line("MIXMAP ", b);
}

// ---------------------------------------------------------------- NFSMixShape

// GetQ15FromHundredthsdB (0x800B592C): hundredths of a dB to a Q15 gain, one
// table octave (6.02 dB) per halving; anything under 0x32 is silence.
static int32_t q15FromDb(int32_t x)
{
	if (x == 0) return 0x7fff;
	// Port guard: retail is only ever handed x <= 0; a positive x would read
	// past its table, and full scale is what a boost can reach anyway.
	if (x > 0) return 0x7fff;
	const int32_t q = x / 602;          // truncates toward 0, as divw does
	const int32_t shift = -q;
	const int32_t rem = q * 602 - x;    // 0 .. 601
	// sraw by 32 or more leaves 0, which C does not promise.
	const int32_t v = shift > 30 ? 0 : (int32_t)kMixDbToQ15[601 - rem] >> shift;
	return v < 0x32 ? 0 : v;
}

// GetdBFromQ15 (0x800B599C): the top set bit picks the octave, the next nine
// bits (padded with ones below bit 5) the table entry.
static int32_t dbFromQ15(int32_t p)
{
	static const int32_t kFill[12] = {   // 0x8031FF50
		0, 1, 3, 7, 0xf, 0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff
	};
	// Port guard: the callers stay inside 0 .. 0x7fff; outside it retail's
	// index runs off its table.
	if (p <= 0) return -10000;
	if (p > 0x7fff) p = 0x7fff;
	int n = 0;
	while (n < 15 && !(p & (0x4000 >> n))) n++;   // 0x4000 >> 15 is 0: n stops at 14
	if (n == 15) return -10000;
	const int32_t m = p - (0x4000 >> n);
	const int32_t idx = n < 5 ? m >> (5 - n) : (m << (n - 5)) | kFill[n - 5];
	const int32_t r = (int32_t)kMixQ15ToDb[idx] - (n + 1) * 602;
	return r > -10001 ? r : -10000;
}

// GetCurveOutput (0x800B5AEC): table `t` at x (0 .. 0x7fff), as Q15, or with
// `dB` as hundredths of a dB. 0 is a quarter cosine falling from full scale, 4
// its mirror rising from 0, 2 and 6 their squares, 8 a straight line down; the
// odd ones are the even one below them run backwards (in dB, negated instead).
static int32_t curveOut(int t, int32_t x, bool dB)
{
	// Port guard: every caller hands in 0 .. 0x7fff; past it the tables end.
	if (x < 0) x = 0;
	if (x > 0x7fff) x = 0x7fff;
	for (;;) {
		const int32_t i = x >> 6;
		// The fraction takes bits 0..4 only, bit 5 skipped, as retail does.
		const int32_t frac = ((x & 0x1f) << 9) | 0x3ff;
		int32_t v;
		switch (t) {
		case 0:
			v = kMixCos[i];
			if (v != 0) v += (((int32_t)kMixCos[i + 1] - v) * frac) >> 15;
			break;
		case 4: {
			const int32_t j = 0x1ff - i;
			v = (0x7fff - (int32_t)kMixCos[j]) +
			    ((((int32_t)kMixCos[j] - (int32_t)kMixCos[j + 1]) * frac) >> 15);
			break;
		}
		case 2:
			if (!dB) { v = curveOut(0, x, false); return (v * v) >> 15; }
			v = curveOut(0, x, true) * 2;
			return v > -0x2581 ? v : -10000;
		case 6:
			if (!dB) { v = curveOut(4, x, false); return (v * v) >> 15; }
			v = curveOut(4, x, true) * 2;
			return v < -0x2580 ? -10000 : v;
		case 8:
			return dB ? dbFromQ15(0x7fff - x) : 0x7fff - x;
		case 1: case 3: case 5: case 7: case 9:
			if (dB) return -curveOut(t - 1, x, true);
			t -= 1;
			x = 0x7fff - x;
			continue;
		default:
			return 0;
		}
		if (dB) {
			v = dbFromQ15(v);
			if (v < -0x2580) v = -10000;
		}
		return v;
	}
}

// GetAzimShapeOutput (0x800B5D34): the two distance curves blended by the
// angle into the quadrant (0 .. 0x3fff), in dB. Retail reads both distances
// through `cA`'s table; `cB` is passed and never used.
static int32_t azimShape(int cA, int cB, const int32_t x[2], int32_t az)
{
	(void)cB;
	const int32_t a = curveOut(cA, x[0], false);
	const int32_t b = az != 0 ? curveOut(cA, x[1], false) : 0x7fff;
	return dbFromQ15(((a * (0x7fff - az * 2)) >> 15) + ((b * az * 2) >> 15));
}

// bASin (0x800594C8): the bAngle (0x4000 a quarter turn) whose sine is x,
// interpolated in bASinTable. x in 1/65536ths falls in one of 12 segments,
// each half the one before it toward x = 1, of 16 entries each.
static uint16_t bASin(float x)
{
	bool neg = false;
	if (x < 0.0f) { x = -x; neg = true; }
	if (x >= 1.0f) return neg ? 0xc000 : 0x4000;
	const int32_t xi = (int32_t)(x * 65536.0f);
	int32_t len = 0x8000, end = 0x8000, seg = 0;
	if (xi >= len) {
		for (int n = 0; n < 11; n++) {
			len >>= 1;
			seg++;
			end += len;
			if (xi < end) break;
		}
	}
	const int32_t start = end - len;
	const int32_t sub = (xi - start) >> (11 - seg);
	const int32_t x0 = start + sub * (len >> 4);
	const MixASinEntry* e = &kMixASin[seg * 16 + sub];
	const float dx = x - (float)x0 * (1.0f / 65536.0f);
	uint16_t r = (uint16_t)(e->angle + (int32_t)(dx * e->slope * 65536.0f));
	return neg ? (uint16_t)-r : r;
}

// ---------------------------------------------------------------- objects
//
// The SFX and SFXCTL objects the map reads and feeds, which retail finds
// through EAXSound::GetSFXBase_Object (0x8007B118, class 4) and
// GetSndBase_Object (0x8007B1A4, class 6): the state's manager, then the copy
// (ID bits 11..15), then the object (bits 4..10).

typedef struct {
	uint32_t    id;         // its retail ID & 0xE0FFFFF0, copy 0
	const char* name;
	int32_t     out[16];    // +4: its outputs, out[n] at +4 + 4n
	int32_t*    slot[16];   // +0x58: an SFX's master channel output words
	int32_t     f98;        // +0x98: an SFX's master channel base, when it has no 3D input
	float       pos[2];     // +0x78: an SFXCTL_3DObjPos's object, where it is (x, y)
	// An SFXCTL_3DObjPos's UpdateDoppler (0x80097B74) state (#503): the
	// distance it follows this update and the last (+0xA0 / +0xA4, -1 at
	// first), and the Average(4) of the factors (+0xA8: samples, count, next
	// slot; +0xB4 their mean).
	float       dopCur, dopPrev;
	float       dopHist[4];
	uint8_t     dopN, dopI;
	float       dopAvg;
	// An SFXCTL_3DObjPos whose position pointer (+0x70) is NULL: Generate3DParams
	// (0x80097980) leaves its outputs as they were. The world's fountains',
	// planes' and trains' (#509) between attachments.
	bool        noPos;
} MixObj;

enum {
	OBJ_MASTERVOL, OBJ_MUSIC, OBJ_SPEECH, OBJ_3DCARPOS, OBJ_NITROUS,
	OBJ_PHYSICS, OBJ_SHIFTING, OBJ_ACCELTRANS, OBJ_ENGINE, OBJ_HYBRIDMOTOR, OBJ_PLAYERENGINE,
	OBJ_SHIFT, OBJ_TURBO, OBJ_SKIDCTL, OBJ_RWHEELPOS, OBJ_LWHEELPOS, OBJ_SKIDS,
	OBJ_ROADNOISE, OBJ_WINDNOISE, OBJ_BOTTOMOUT, OBJ_TUNNEL, OBJ_3DTUNNELPOS, OBJ_DRIVEBY,
	OBJ_WINDWEATHER, OBJ_SPARKCHATTER, OBJ_COMMON, OBJ_RAIN, OBJ_HYDRAULICS,
	OBJ_AI3DCARPOS,                            // one per AICar copy ...
	OBJ_AIENGINE = OBJ_AI3DCARPOS + MIXMAP_AI_COPIES,
	OBJ_AITURBO = OBJ_AIENGINE + MIXMAP_AI_COPIES,
	OBJ_AINITROUS = OBJ_AITURBO + MIXMAP_AI_COPIES,
	OBJ_AISKIDS = OBJ_AINITROUS + MIXMAP_AI_COPIES,
	OBJ_AISTEREO = OBJ_AISKIDS + MIXMAP_AI_COPIES,
	OBJ_COLLISION = OBJ_AISTEREO + MIXMAP_AI_COPIES, // ... and per Collision copy
	OBJ_3DCOLPOS = OBJ_COLLISION + MIXMAP_COL_COPIES,
	OBJ_3DSCRAPEPOS = OBJ_3DCOLPOS + MIXMAP_COL_COPIES,
	OBJ_WOOSH = OBJ_3DSCRAPEPOS + MIXMAP_COL_COPIES,   // ... and per DriveBy copy
	OBJ_3DWOOSHPOS = OBJ_WOOSH + MIXMAP_DRIVEBY_COPIES,
	OBJ_3DTRAFFICPOS = OBJ_3DWOOSHPOS + MIXMAP_DRIVEBY_COPIES,   // ... and per Traffic copy
	OBJ_TRAFFICENGINE = OBJ_3DTRAFFICPOS + MIXMAP_TRAFFIC_COPIES,
	OBJ_TRAFFICWOOSH = OBJ_TRAFFICENGINE + MIXMAP_TRAFFIC_COPIES,
	OBJ_TRAFFICHORN = OBJ_TRAFFICWOOSH + MIXMAP_TRAFFIC_COPIES,
	OBJ_TRAFFICSTEREO = OBJ_TRAFFICHORN + MIXMAP_TRAFFIC_COPIES,
	OBJ_FOUNTAIN = OBJ_TRAFFICSTEREO + MIXMAP_TRAFFIC_COPIES,   // ... and the world's (#509)
	OBJ_3DFOUNTAINPOS = OBJ_FOUNTAIN + MIXMAP_FOUNTAIN_COPIES,
	OBJ_PLANE = OBJ_3DFOUNTAINPOS + MIXMAP_FOUNTAIN_COPIES,
	OBJ_3DPLANEPOS = OBJ_PLANE + MIXMAP_PLANE_COPIES,
	OBJ_TRAIN = OBJ_3DPLANEPOS + MIXMAP_PLANE_COPIES,
	OBJ_3DTRAINPOS = OBJ_TRAIN + MIXMAP_TRAIN_COPIES,
	OBJ_FRONTEND = OBJ_3DTRAINPOS + MIXMAP_TRAIN_COPIES,   // EAXFrontEnd (#517)
	OBJ_COUNT
};

// The PlayerCar state's engine controllers write their own outputs here
// (enginectl.c, through mixmapObjOut); Shifting's are all master volumes,
// which shiftingObjUpdate fills in.
static MixObj s_obj[OBJ_COUNT] = {
	{ 0x60000000u, "SFXCTL_MasterVol" },     // Main state, SFXCTL 0
	{ 0x40000000u, "SFXObj_Music" },         // Main state, SFX 0 (reads only: its UpdateParams 0x800BEA64 is empty)
	{ 0x40000010u, "SFXObj_Speech" },        // Main state, SFX 1
	{ 0x60010080u, "SFXCTL_3DCarPos" },      // PlayerCar state, SFXCTL 8
	{ 0x40010030u, "CARSFX_Nitrous" },       // PlayerCar state, SFX 3
	{ 0x60010000u, "SFXCTL_Physics" },       // PlayerCar state, SFXCTL 0
	{ 0x60010030u, "SFXCTL_Shifting" },      // PlayerCar state, SFXCTL 3
	{ 0x60010040u, "SFXCTL_AccelTrans" },    // PlayerCar state, SFXCTL 4
	{ 0x60010050u, "SFXCTL_Engine" },        // PlayerCar state, SFXCTL 5
	{ 0x60010060u, "SFXCTL_HybridMotor" },   // PlayerCar state, SFXCTL 6
	{ 0x40010000u, "CARSFX_PlayerEngine" },  // PlayerCar state, SFX 0
	{ 0x40010010u, "CARSFX_Shift" },         // PlayerCar state, SFX 1
	{ 0x40010020u, "CARSFX_Turbo" },         // PlayerCar state, SFX 2
	{ 0x60010020u, "SFXCTL_Skid" },          // PlayerCar state, SFXCTL 2 (carsfx.c writes it)
	{ 0x600100b0u, "SFXCTL_3DRightWheelPos" }, // PlayerCar state, SFXCTL 11
	{ 0x600100c0u, "SFXCTL_3DLeftWheelPos" },  // PlayerCar state, SFXCTL 12
	{ 0x40010050u, "CARSFX_Skids" },         // PlayerCar state, SFX 5
	{ 0x40010060u, "CARSFX_RoadNoise" },     // PlayerCar state, SFX 6
	{ 0x40010070u, "CARSFX_WindNoise" },     // PlayerCar state, SFX 7
	{ 0x400100b0u, "CARSFX_BottomOut" },     // PlayerCar state, SFX 11
	{ 0x60010070u, "SFXCTL_Tunnel" },        // PlayerCar state, SFXCTL 7 (carsfx.c writes it)
	{ 0x600100a0u, "SFXCTL_3DTunnelPos" },   // PlayerCar state, SFXCTL 10
	{ 0x400100c0u, "CARSFX_DriveBy" },       // PlayerCar state, SFX 12
	{ 0x400100a0u, "CARSFX_WindWeather" },   // PlayerCar state, SFX 10
	{ 0x40010040u, "CARSFX_SparkChatter" },  // PlayerCar state, SFX 4
	{ 0x40000040u, "SFX_Common" },           // Main state, SFX 4 (its FX_Camera, colsfx.c)
	{ 0x40010090u, "CARSFX_Rain" },          // PlayerCar state, SFX 9
	{ 0x40010080u, "CARSFX_Hydraulics" },    // PlayerCar state, SFX 8 (#313)
	// The AICar state's (2), a copy per opponent (ID bits 11..15): its
	// SFXCTL_3DCarPos (8), where the opponent is, CARSFX_AIEngine (SFX 0), and
	// the PlayerCar state's CARSFX_Turbo (2), CARSFX_Nitrous (3) and
	// CARSFX_Skids (5), which CreateSFX falls back to, and its own
	// CARSFX_Stereo (15).
	{ 0x60020080u, "SFXCTL_3DCarPos (AI 0)" },
	{ 0x60020880u, "SFXCTL_3DCarPos (AI 1)" },
	{ 0x60021080u, "SFXCTL_3DCarPos (AI 2)" },
	{ 0x60021880u, "SFXCTL_3DCarPos (AI 3)" },
	{ 0x60022080u, "SFXCTL_3DCarPos (AI 4)" },
	{ 0x40020000u, "CARSFX_AIEngine (AI 0)" },
	{ 0x40020800u, "CARSFX_AIEngine (AI 1)" },
	{ 0x40021000u, "CARSFX_AIEngine (AI 2)" },
	{ 0x40021800u, "CARSFX_AIEngine (AI 3)" },
	{ 0x40022000u, "CARSFX_AIEngine (AI 4)" },
	{ 0x40020020u, "CARSFX_Turbo (AI 0)" },
	{ 0x40020820u, "CARSFX_Turbo (AI 1)" },
	{ 0x40021020u, "CARSFX_Turbo (AI 2)" },
	{ 0x40021820u, "CARSFX_Turbo (AI 3)" },
	{ 0x40022020u, "CARSFX_Turbo (AI 4)" },
	{ 0x40020030u, "CARSFX_Nitrous (AI 0)" },
	{ 0x40020830u, "CARSFX_Nitrous (AI 1)" },
	{ 0x40021030u, "CARSFX_Nitrous (AI 2)" },
	{ 0x40021830u, "CARSFX_Nitrous (AI 3)" },
	{ 0x40022030u, "CARSFX_Nitrous (AI 4)" },
	{ 0x40020050u, "CARSFX_Skids (AI 0)" },
	{ 0x40020850u, "CARSFX_Skids (AI 1)" },
	{ 0x40021050u, "CARSFX_Skids (AI 2)" },
	{ 0x40021850u, "CARSFX_Skids (AI 3)" },
	{ 0x40022050u, "CARSFX_Skids (AI 4)" },
	{ 0x400200f0u, "CARSFX_Stereo (AI 0)" },
	{ 0x400208f0u, "CARSFX_Stereo (AI 1)" },
	{ 0x400210f0u, "CARSFX_Stereo (AI 2)" },
	{ 0x400218f0u, "CARSFX_Stereo (AI 3)" },
	{ 0x400220f0u, "CARSFX_Stereo (AI 4)" },
	// The Collision state's (5), a copy per sound a collision can hold
	// (colsfx.c): SFXObj_Collision (SFX 0), where the impact was
	// (SFXCTL_3DColPos, 0) and where the scrape is (SFXCTL_3DScrapePos, 1).
	{ 0x40050000u, "SFXObj_Collision (0)" },
	{ 0x40050800u, "SFXObj_Collision (1)" },
	{ 0x40051000u, "SFXObj_Collision (2)" },
	{ 0x40051800u, "SFXObj_Collision (3)" },
	{ 0x40052000u, "SFXObj_Collision (4)" },
	{ 0x60050000u, "SFXCTL_3DColPos (0)" },
	{ 0x60050800u, "SFXCTL_3DColPos (1)" },
	{ 0x60051000u, "SFXCTL_3DColPos (2)" },
	{ 0x60051800u, "SFXCTL_3DColPos (3)" },
	{ 0x60052000u, "SFXCTL_3DColPos (4)" },
	{ 0x60050010u, "SFXCTL_3DScrapePos (0)" },
	{ 0x60050810u, "SFXCTL_3DScrapePos (1)" },
	{ 0x60051010u, "SFXCTL_3DScrapePos (2)" },
	{ 0x60051810u, "SFXCTL_3DScrapePos (3)" },
	{ 0x60052010u, "SFXCTL_3DScrapePos (4)" },
	// The DriveBy state's (6), a copy per woosh at once (carsfx.c): the
	// roadside and passing-car woosh, SFXObj_Woosh (SFX 0), and where its
	// lamp post or car is (SFXCTL_3DWooshPos, 0).
	{ 0x40060000u, "SFXObj_Woosh (0)" },
	{ 0x40060800u, "SFXObj_Woosh (1)" },
	{ 0x40061000u, "SFXObj_Woosh (2)" },
	{ 0x40061800u, "SFXObj_Woosh (3)" },
	{ 0x60060000u, "SFXCTL_3DWooshPos (0)" },
	{ 0x60060800u, "SFXCTL_3DWooshPos (1)" },
	{ 0x60061000u, "SFXCTL_3DWooshPos (2)" },
	{ 0x60061800u, "SFXCTL_3DWooshPos (3)" },
	// The Traffic state's (3), a copy per traffic car within 70 m
	// (carsfx.c): where the car is (SFXCTL_3DTrafficPos, 0), its engine
	// (CARSFX_TrafficEngine, SFX 0), its woosh going by (CARSFX_TrafficWoosh,
	// 1), its horn (CARSFX_TrafficHorn, 2) and its stereo (CARSFX_Stereo, 3).
	{ 0x60030000u, "SFXCTL_3DTrafficPos (0)" },
	{ 0x60030800u, "SFXCTL_3DTrafficPos (1)" },
	{ 0x60031000u, "SFXCTL_3DTrafficPos (2)" },
	{ 0x60031800u, "SFXCTL_3DTrafficPos (3)" },
	{ 0x60032000u, "SFXCTL_3DTrafficPos (4)" },
	{ 0x60032800u, "SFXCTL_3DTrafficPos (5)" },
	{ 0x40030000u, "CARSFX_TrafficEngine (0)" },
	{ 0x40030800u, "CARSFX_TrafficEngine (1)" },
	{ 0x40031000u, "CARSFX_TrafficEngine (2)" },
	{ 0x40031800u, "CARSFX_TrafficEngine (3)" },
	{ 0x40032000u, "CARSFX_TrafficEngine (4)" },
	{ 0x40032800u, "CARSFX_TrafficEngine (5)" },
	{ 0x40030010u, "CARSFX_TrafficWoosh (0)" },
	{ 0x40030810u, "CARSFX_TrafficWoosh (1)" },
	{ 0x40031010u, "CARSFX_TrafficWoosh (2)" },
	{ 0x40031810u, "CARSFX_TrafficWoosh (3)" },
	{ 0x40032010u, "CARSFX_TrafficWoosh (4)" },
	{ 0x40032810u, "CARSFX_TrafficWoosh (5)" },
	{ 0x40030020u, "CARSFX_TrafficHorn (0)" },
	{ 0x40030820u, "CARSFX_TrafficHorn (1)" },
	{ 0x40031020u, "CARSFX_TrafficHorn (2)" },
	{ 0x40031820u, "CARSFX_TrafficHorn (3)" },
	{ 0x40032020u, "CARSFX_TrafficHorn (4)" },
	{ 0x40032820u, "CARSFX_TrafficHorn (5)" },
	{ 0x40030030u, "CARSFX_Stereo (traffic 0)" },
	{ 0x40030830u, "CARSFX_Stereo (traffic 1)" },
	{ 0x40031030u, "CARSFX_Stereo (traffic 2)" },
	{ 0x40031830u, "CARSFX_Stereo (traffic 3)" },
	{ 0x40032030u, "CARSFX_Stereo (traffic 4)" },
	{ 0x40032830u, "CARSFX_Stereo (traffic 5)" },
	// The world's (envsfx.c, #509): the Fountain state's (4) SFXObj_Fountain
	// (SFX 0) and SFXCTL_3DFountainPos (0), the Plane state's (7) SFXObj_Plane
	// and SFXCTL_3DEnviroObjPos, and the Train state's (8) SFXObj_Train and
	// SFXCTL_3DTrainObjPos, a copy per state their managers make.
	{ 0x40040000u, "SFXObj_Fountain (0)" },
	{ 0x40040800u, "SFXObj_Fountain (1)" },
	{ 0x40041000u, "SFXObj_Fountain (2)" },
	{ 0x40041800u, "SFXObj_Fountain (3)" },
	{ 0x60040000u, "SFXCTL_3DFountainPos (0)" },
	{ 0x60040800u, "SFXCTL_3DFountainPos (1)" },
	{ 0x60041000u, "SFXCTL_3DFountainPos (2)" },
	{ 0x60041800u, "SFXCTL_3DFountainPos (3)" },
	{ 0x40070000u, "SFXObj_Plane (0)" },
	{ 0x40070800u, "SFXObj_Plane (1)" },
	{ 0x40071000u, "SFXObj_Plane (2)" },
	{ 0x60070000u, "SFXCTL_3DEnviroObjPos (0)" },
	{ 0x60070800u, "SFXCTL_3DEnviroObjPos (1)" },
	{ 0x60071000u, "SFXCTL_3DEnviroObjPos (2)" },
	{ 0x40080000u, "SFXObj_Train (0)" },
	{ 0x40080800u, "SFXObj_Train (1)" },
	{ 0x40081000u, "SFXObj_Train (2)" },
	{ 0x40081800u, "SFXObj_Train (3)" },
	{ 0x40082000u, "SFXObj_Train (4)" },
	{ 0x40082800u, "SFXObj_Train (5)" },
	{ 0x60080000u, "SFXCTL_3DTrainObjPos (0)" },
	{ 0x60080800u, "SFXCTL_3DTrainObjPos (1)" },
	{ 0x60081000u, "SFXCTL_3DTrainObjPos (2)" },
	{ 0x60081800u, "SFXCTL_3DTrainObjPos (3)" },
	{ 0x60082000u, "SFXCTL_3DTrainObjPos (4)" },
	{ 0x60082800u, "SFXCTL_3DTrainObjPos (5)" },
	{ 0x40000060u, "SFXObj_FrontEnd" },    // Main state, SFX 6 (reads only: the purge's level, #517)
};

// SFXCTL_3DObjPos keeps, past its four outputs, the camera it listens with
// (+0x30) and, from its 3D controls, the camera's farthest reach (+0x34) and
// its record's +0 bits 8..15 (+0x38).
enum { POS3D_CAMERA = 11, POS3D_MAXDIST = 12, POS3D_CAMFLAGS = 13 };

// Every object the file names that the port has not ported. Port stand-in:
// retail always has the object, so its outputs (the engine's revs, a horn's
// playing flag, ...) are real values; here they read 0 -- not playing, no
// level -- which leaves its envelopes idle and its data procs at rest.
// Retail's own fallback for a missing object, nUnity, would be wrong here:
// as a trigger it holds an envelope on for good.
static MixObj s_stub = { 0, "(not ported)" };

static const int32_t s_unity = 0x7fff;   // nUnity, r13 - 0x75D0
static const int32_t s_zero[2] = { 0, 0 };

static MixObj* objFind(uint32_t id)
{
	const uint32_t key = id & 0xe0fffff0u;
	for (int i = 0; i < OBJ_COUNT; i++)
		if (s_obj[i].id == key) return &s_obj[i];
	return NULL;
}

static MixObj* objRead(uint32_t id)
{
	MixObj* o = objFind(id);
	return o ? o : &s_stub;
}

// ---------------------------------------------------------------- map pieces

typedef struct {              // NFSMixDataProc
	uint32_t       id;        // +4: what it reads, its curve in bits 24..28
	int            curve;     // +0xC
	const int32_t* in;        // +0x10
	int32_t        dB;        // +0x14
	int32_t        q15;       // +0x18
} DataProc;

typedef struct {              // NFSMixCtl
	const uint32_t* rec;      // +4
	uint32_t        id;       // +0xC
	uint32_t        scaleId;  // +0x10
	DataProc*       dp;       // +0x18
	int32_t         out;      // +0x1C, hundredths of a dB
	const int32_t*  scale;    // +0x20
	int32_t         offset;   // +0x24
	int32_t         ratio;    // +0x28
} MixCtl;

typedef struct {              // NFSEvtMixCtl
	int             active;   // +4
	int32_t         start;    // +8
	const int32_t*  trig;     // +0xC
	const int32_t*  scale;    // +0x10
	int32_t         range;    // +0x14
	int32_t         out;      // +0x18
	int32_t         env;      // +0x1C
	const uint32_t* rec;      // +0x20
	float           timer;    // +0x24, ms
	float           mark;     // +0x28, ms: when the trigger let go
} EvtCtl;

typedef struct {              // NFS3DMixCtl
	MixObj*         pos;      // +4 (retail keeps &out[0]): the SFXCTL_3DObjPos it reads
	int32_t         shaped;   // +8
	int32_t         out[2];   // +0x10 dB, +0x14 azimuth: what a master channel reads
	uint32_t        hdr;      // +0x1C
	uint32_t        id;       // +0x20
	const uint32_t* cam;      // +0x24: the camera record in use
	const uint32_t* rec;      // +0x28
} Ctl3D;

typedef struct {              // NFSMixChannel / NFSMasterMixChannel
	int              connected;  // +4
	uint32_t         hdr;        // +8
	uint32_t         id;         // +0xC
	int32_t          value;      // +0x10, hundredths of a dB
	int32_t          base;       // +0x18
	int32_t          max;        // +0x1C
	int32_t          min;        // +0x20
	int              nFixed;     // +0x28
	int              n3D;        // +0x2C
	int              nInputs;    // +0x30
	const uint32_t*  inMap;      // +0x34
	const int32_t**  fixed;      // +0x38
	const int32_t**  in3D;       // +0x3C
	uint32_t         outId;      // master: the SFX it feeds
	int              group;      // +0x48
	int32_t*         block;      // +0x4C: the group's 16 output words
	const uint32_t*  outMap;     // +0x50
	MixObj*          sfx;        // the port's note of who reads the block
} Channel;

typedef struct {              // NFSMixMapProc, one copy of one main map state
	bool            live;
	int             state;       // +0x44
	int             copy;        // +0x50
	const uint8_t*  blk;         // +8: the state's block of the file
	int             nDp;  DataProc* dp;       // +0x58, +0x2C
	int             nCtl; MixCtl*   ctl;      // +0x5C, +0x20
	int             nEvt; EvtCtl*   evt;      // +0x64, +0x28
	int             n3D;  Ctl3D*    c3d;      // +0x60, +0x24
	int             nSub; Channel*  sub;      // +0x68, +0x30
	int             nMaster; Channel* master; // +0x6C, +0x34
	int             nGroups; int32_t* outBlock; // +0x40: 16 words a group
	const uint32_t* outMaps;     // the state's +0x14 section
} MixState;

enum { MIX_STATES = 9 };

// CSTATEMGR_*, by eMAINMAPSTATES.
static const char* const kStateName[MIX_STATES] = {
	"Main", "PlayerCar", "AICar", "Traffic", "Fountain", "Collision", "DriveBy", "Plane", "Train"
};

// The most copies the port makes of a state: the AICar state's, one per
// opponent, the Collision state's, the DriveBy state's and the Traffic
// state's.
#define MIX_MAX2(a, b) ((a) > (b) ? (a) : (b))
enum { MIX_COPIES = MIX_MAX2(MIX_MAX2(MIX_MAX2(MIX_MAX2(MIXMAP_AI_COPIES, MIXMAP_COL_COPIES),
	MIXMAP_DRIVEBY_COPIES), MIXMAP_TRAFFIC_COPIES), MIXMAP_TRAIN_COPIES) };

static struct {
	bool     ready;
	uint8_t* file;
	uint32_t size;
	int32_t  mapId;               // +0x2C
	int      copies[MIX_STATES];  // +4
	MixState st[MIX_STATES][MIX_COPIES];   // [state][copy], the copies the port makes
	float    reportIn;
} s_map;

// SndCamera (0x8008DB18 UpdateCameras), camera 0: the single-player one.
static struct {
	float normCamDir[2], avgCamDir[2], worldCamPos[2], worldCarPos[2], normCarDir[2];
	float carPos[2];   // the 3DCarPos object's own position (+0x78)
	int   cur, prev;   // eCamStates
	bool  valid;
} s_cam;

// ---------------------------------------------------------------- the file

static bool span(const void* p, uint32_t words)
{
	const uint8_t* b = (const uint8_t*)p;
	return b >= s_map.file && ((uintptr_t)(b - s_map.file) & 3) == 0 &&
	       (uint32_t)(b - s_map.file) <= s_map.size &&
	       words <= (s_map.size - (uint32_t)(b - s_map.file)) / 4;
}

// The state block's section `field` (+4 MixCtls ... +0x18 event MixCtls),
// NULL for -1 or one that does not fit.
static const uint32_t* section(const MixState* s, int field)
{
	const int32_t off = ((const int32_t*)s->blk)[field];
	if (off == -1) return NULL;
	const uint32_t* p = (const uint32_t*)(s->blk + off);
	// Port guard: four words, except the output maps (5), which
	// initMasterChannels measures entry by entry -- the Train state's (#509)
	// is the file's last two words.
	return span(p, field == 5 ? 1 : 4) ? p : NULL;
}

// The state and copy of `id` (bits 16..23, 11..15), if the port made it.
static MixState* stateOf(uint32_t id, int copy)
{
	const uint32_t st = (id >> 16) & 0xff;
	if (st >= MIX_STATES || copy < 0 || copy >= s_map.copies[st] || !s_map.st[st][copy].live)
		return NULL;
	return &s_map.st[st][copy];
}

static int copiesOf(uint32_t st)
{
	return st < MIX_STATES ? s_map.copies[st] : 0;
}

// NFSMixMap::GetStateMixCtl / EvtMixCtl / SubMix / MasterMix (0x800B5058 on).
static MixCtl* stateMixCtl(uint32_t id, int copy)
{
	MixState* s = stateOf(id, copy);
	return s && (int)(id & 0xff) < s->nCtl ? &s->ctl[id & 0xff] : NULL;
}

static EvtCtl* stateEvt(uint32_t id, int copy)
{
	MixState* s = stateOf(id, copy);
	return s && (int)(id & 0xff) < s->nEvt ? &s->evt[id & 0xff] : NULL;
}

static Channel* stateSub(uint32_t id, int copy)
{
	MixState* s = stateOf(id, copy);
	return s && (int)(id & 0xff) < s->nSub ? &s->sub[id & 0xff] : NULL;
}

static Channel* stateMaster(uint32_t id, int copy)
{
	MixState* s = stateOf(id, copy);
	return s && (int)(id & 0xff) < s->nMaster ? &s->master[id & 0xff] : NULL;
}

// NFSMixMapProc::Get3DMixCtl (0x800B2E44): the one whose ID is `id`, else the
// one at its low byte (port guard: none past the end).
static Ctl3D* get3DMixCtl(MixState* s, uint32_t id)
{
	for (int i = 0; i < s->n3D; i++)
		if (s->c3d[i].id == id) return &s->c3d[i];
	return (int)(id & 0xff) < s->n3D ? &s->c3d[id & 0xff] : NULL;
}

// ---------------------------------------------------------------- creation

// NFSMixDataProc::Initialize (0x800B1D64).
static void dataProcInit(DataProc* d, uint32_t id)
{
	d->id = id;
	d->curve = (int)((id >> 24) & 0x1f);
	d->in = NULL;
	d->dB = 0;
	d->q15 = 0;
}

// NFSMixCtl::Initialize (0x800B1B98) and GetMixScaleRatio (0x800B1C4C).
// Record: +0 what its data proc reads, +4 the range, +8 the scale.
static void mixCtlInit(MixCtl* c, DataProc* d, const uint32_t* rec, int index, int32_t mapId)
{
	c->rec = rec;
	c->dp = d;
	c->scaleId = rec[2];
	c->out = 0;
	c->id = (rec[0] & 0x1fff0000u) | ((rec[0] >> 16) & 0xe000u) | (uint32_t)index |
	        ((uint32_t)mapId << 8);
	int32_t range;
	if (!(rec[1] & 0x8000u)) {
		c->offset = (int32_t)(rec[1] & 0x7fff);
		range = -c->offset;
	} else {
		c->offset = 0;
		range = (int32_t)((rec[1] & 0xffffu) | 0xffff0000u);   // GC's halfword at +6
	}
	c->ratio = 0x7fff - q15FromDb(range);
}

// NFSMixMapProc::CreateMixCtls (0x800B3300): each MixCtl gets a data proc of
// its own, past the first `shared` ones, which come from the master map.
static bool createMixCtls(MixState* s)
{
	const uint32_t* sec = section(s, 1);
	if (!sec || (int32_t)sec[0] <= 0) return true;
	const int n = (int32_t)sec[0], nDp = (int32_t)sec[1], shared = (int32_t)sec[2];
	// MAPOUTPUT has no shared data procs, and no master map to share them from.
	if (shared != 0 || nDp < n || !span(sec + 4, (uint32_t)n * 3)) {
		mixLog("%s: MixCtl section %d/%d/%d not as expected", kStateName[s->state], n, nDp, shared);
		return false;
	}
	s->ctl = calloc((size_t)n, sizeof(MixCtl));
	s->dp = calloc((size_t)nDp, sizeof(DataProc));
	if (!s->ctl || !s->dp) return false;
	const uint32_t* rec = sec + 4;
	for (int i = 0; i < n; i++, rec += 3) {
		DataProc* d = &s->dp[s->nDp++];
		dataProcInit(d, rec[0] | (s->copy ? (uint32_t)s->copy << 11 : 0));
		mixCtlInit(&s->ctl[s->nCtl++], d, rec, i, s_map.mapId);
	}
	return true;
}

// NFS3DMixCtl::Initialize (0x800B5E08). Record: a header, its camera count in
// bits 24..27, then that many six-word camera records.
static void ctl3DInit(Ctl3D* c, const uint32_t* rec, uint32_t copyBits, int index)
{
	memset(c, 0, sizeof(*c));
	c->rec = rec;
	c->cam = rec + 1;
	c->hdr = copyBits | rec[0];
	c->id = (c->hdr & 0xff0000u) | copyBits | (uint32_t)index | 0x90000000u;
	c->out[0] = -10000;
}

static bool create3DMixCtls(MixState* s)
{
	const uint32_t* sec = section(s, 2);
	if (!sec || (int32_t)sec[0] <= 0) return true;
	const int n = (int32_t)sec[0];
	s->c3d = calloc((size_t)n, sizeof(Ctl3D));
	if (!s->c3d) return false;
	const uint32_t* rec = sec + 4;
	for (int i = 0; i < n; i++) {
		const uint32_t ncam = span(rec, 1) ? (rec[0] >> 24) & 0xf : 0;
		// Port guard: a control with no camera would read the next record as one.
		if (ncam == 0 || !span(rec, 1 + ncam * 6)) {
			mixLog("%s: 3D MixCtl %d malformed", kStateName[s->state], i);
			return false;
		}
		ctl3DInit(&s->c3d[s->n3D++], rec, (uint32_t)s->copy << 11, i);
		rec += 1 + ncam * 6;
	}
	return true;
}

// NFSEvtMixCtl::Initialize (0x800B6544). Record: +0 its kind in bits 24..27,
// +4 the range, +8 the scale, +0xC the trigger, +0x10 / +0x14 / +0x18 the
// attack, sustain and release (frames in bits 0..11, a curve in 12..15).
static bool createEvtMixCtls(MixState* s)
{
	const uint32_t* sec = section(s, 6);
	if (!sec || (int32_t)sec[0] <= 0) return true;
	const int n = (int32_t)sec[0];
	if (!span(sec + 4, (uint32_t)n * 7)) return false;
	s->evt = calloc((size_t)n, sizeof(EvtCtl));
	if (!s->evt) return false;
	const uint32_t* rec = sec + 4;
	for (int i = 0; i < n; i++, rec += 7) {
		EvtCtl* e = &s->evt[s->nEvt++];
		e->rec = rec;
		e->timer = 0.0f;
		const uint32_t r = rec[1] & 0xffff;   // GC's halfword at +6
		e->range = r < 10001 ? (int32_t)r : (int32_t)(rec[1] | 0xffff0000u);
	}
	return true;
}

// NFSMixChannel::Initialize (0x800B1914) and NFSMixMapProc::InitChannel
// (0x800B50F8). A master record is its header, +4 its headroom (the upper
// half, GC's halfword at +4) and +8 its SFX; a sub's +4 has its ceiling in the
// upper half and its floor in the lower. The inputs follow.
static const uint32_t* channelInit(Channel* ch, const uint32_t* rec)
{
	memset(ch, 0, sizeof(*ch));
	const uint32_t h = rec[0];
	ch->hdr = h;
	ch->id = (h & 0x10000000u) | ((h & 0xff00u) << 8) | 0x20000000u | (h & 0xff);
	ch->nInputs = (int)((h >> 16) & 0xff);
	if (!(h & 0x10000000u)) {
		const uint32_t up = rec[1] >> 16;
		ch->max = (int32_t)up;
		ch->base = -(int32_t)(int16_t)up;
		ch->min = ch->base - 10000;
		ch->outId = rec[2];
		ch->inMap = rec + 3;
	} else {
		ch->base = 0;
		ch->max = (int32_t)((rec[1] >> 16) & 0x7fff);
		ch->min = (int32_t)(rec[1] | 0xffff0000u);
		ch->inMap = rec + 2;
	}
	return ch->inMap + ch->nInputs;
}

// CreateSubMixChannels (0x800B362C) / CreateMixChannels (0x800B37D4).
static bool createChannels(MixState* s, int field, Channel** out, int* count)
{
	const uint32_t* sec = section(s, field);
	if (!sec) return true;
	if (field == 4) {
		// The output blocks: the section's +4 groups of 16 words.
		s->nGroups = (int32_t)sec[1];
		if (s->nGroups < 0 || s->nGroups > 64) return false;
		s->outBlock = calloc((size_t)(s->nGroups ? s->nGroups : 1) * 16, sizeof(int32_t));
		if (!s->outBlock) return false;
	}
	const int n = (int32_t)sec[0];
	if (n <= 0) return true;
	*out = calloc((size_t)n, sizeof(Channel));
	if (!*out) return false;
	const uint32_t* rec = sec + 4;
	for (int i = 0; i < n; i++) {
		if (!span(rec, 3)) return false;
		rec = channelInit(&(*out)[(*count)++], rec);
		if (!span(rec, 0)) return false;
	}
	return true;
}

// NFSMixMap::CreateMainMapState (0x800B26E0).
static bool createState(int st, int copy)
{
	MixState* s = &s_map.st[st][copy];
	const int32_t* hdr = (const int32_t*)s_map.file;
	const int32_t tableOff = hdr[2];
	const int32_t* table = (const int32_t*)(s_map.file + tableOff);
	if (st >= hdr[1] || !span(table + st, 1) || table[st] == -1 ||
	    !span(s_map.file + table[st], 7)) {
		mixLog("%s: no state block", kStateName[st]);
		return false;
	}
	s->live = true;
	s->state = st;
	s->copy = copy;
	s->blk = s_map.file + table[st];
	const uint32_t* maps = section(s, 5);
	s->outMaps = maps ? maps : NULL;
	return createMixCtls(s) && create3DMixCtls(s) && createEvtMixCtls(s) &&
	       createChannels(s, 3, &s->sub, &s->nSub) &&
	       createChannels(s, 4, &s->master, &s->nMaster);
}

// SetNumFixedInputs (0x800B19AC): the input array, the 3D ones after the rest.
static bool channelInputs(Channel* ch, int nFixed, int n3D)
{
	ch->n3D = n3D;
	ch->nFixed = nFixed;
	const int total = nFixed + n3D;
	ch->fixed = calloc((size_t)(total ? total : 1), sizeof(const int32_t*));
	if (!ch->fixed) return false;
	ch->in3D = n3D > 0 ? ch->fixed + nFixed : NULL;
	return true;
}

// NFSMixMapProc::InitializeSubChannels (0x800B3918): an input of another state
// counts once per copy of it; a 3D one only in this state.
static bool initSubChannels(MixState* s)
{
	for (int i = 0; i < s->nSub; i++) {
		Channel* ch = &s->sub[i];
		int nFixed = 0, n3D = 0;
		for (int k = 0; k < ch->nInputs; k++) {
			const uint32_t id = ch->inMap[k];
			const uint32_t st = (id >> 16) & 0xff;
			if ((int)st == s->state) {
				if ((id & 0xe0000000u) == 0x80000000u) n3D++;
				else nFixed++;
			} else {
				nFixed += copiesOf(st);
			}
		}
		if (!channelInputs(ch, nFixed, n3D)) return false;
	}
	return true;
}

// NFS3DMixCtl::SetupMixConnections (0x800B5E54) and the camera part of
// ChangeCameraState (0x800B5ED4): the farthest reach of the record's four
// quadrants (their maxima, GC's halfwords at +8 ... +0x14) and its +0 flags.
static void ctl3DTellPos(Ctl3D* c)
{
	uint32_t far = 0;
	for (int q = 2; q < 6; q++) {
		const uint32_t m = (c->cam[q] >> 16) & 0x7fff;
		if (far < m) far = m;
	}
	c->pos->out[POS3D_MAXDIST] = (int32_t)far;
	c->pos->out[POS3D_CAMFLAGS] = (int32_t)(c->cam[0] & 0xff00);
}

// NFSMixMapProc::InitializeMasterChannels (0x800B3A7C): the output group (one
// per SFX, shared by the channels that feed it), the output map, the inputs,
// and then the SFX's slots pointed at the group's words. A channel whose SFX
// is missing is never processed.
static bool initMasterChannels(MixState* s)
{
	const uint32_t* outMap = s->outMaps;
	for (int i = 0; i < s->nMaster; i++) {
		Channel* ch = &s->master[i];
		// The group: how many times the SFX changed, in channel order, before
		// the first channel feeding this one's.
		bool found = false;
		int cnt = 0;
		uint32_t prev = 0;
		for (int j = 0; j < s->nMaster; j++) {
			const uint32_t o = s->master[j].outId;
			if ((o & 0xfffffff0u) == (ch->outId & 0xfffffff0u)) {
				if (!found) { ch->group = cnt; found = true; }
				if (o == ch->outId) break;
			} else if (prev != o) {
				cnt++;
			}
			prev = o;
		}
		if ((ch->hdr & 0xff000000u) == 0xc0000000u) {
			if (ch->group >= s->nGroups) {
				mixLog("%s master %d: group %d of %d", kStateName[s->state], i, ch->group, s->nGroups);
				return false;
			}
			ch->block = s->outBlock + ch->group * 16;
		}
		if (!outMap || !span(outMap, 1) || !span(outMap, 1 + (outMap[0] & 0xf))) return false;
		ch->outMap = outMap;
		outMap += 1 + (outMap[0] & 0xf);

		int nFixed = 0, n3D = 0;
		for (int k = 0; k < ch->nInputs; k++) {
			const uint32_t id = ch->inMap[k];
			const uint32_t st = (id >> 16) & 0xff;
			if ((id & 0xe0000000u) == 0x80000000u) n3D++;
			else if ((int)st == s->state) nFixed++;
			else nFixed += copiesOf(st);
		}
		if (!channelInputs(ch, nFixed, n3D)) return false;

		MixObj* sfx = objFind((ch->outId & 0xe0ffffffu) | ((uint32_t)s->copy << 11));
		if (!sfx) {
			ch->block = NULL;
			continue;
		}
		ch->sfx = sfx;
		const uint32_t* in = ch->inMap;
		if (ch->n3D > 0) {
			for (int k = 0; k < ch->n3D; ) {
				const uint32_t id = *in++ | ((uint32_t)s->copy << 11);
				Ctl3D* c = get3DMixCtl(s, id);
				if (!c) break;
				ch->in3D[k++] = c->out;
				// The SFXCTL_3DObjPos the control reads. Port guard: retail always
				// has it; a missing one leaves the control unconnected and idle.
				MixObj* pos = objFind((c->hdr & 0x1fffffffu) | 0x60000000u);
				if (pos) {
					c->pos = pos;
					ctl3DTellPos(c);
				}
			}
		}
		if (ch->block) {
			const uint32_t n = ch->outMap[0] & 0xf;
			for (uint32_t k = 1; k <= n; k++) {
				const uint32_t e = ch->outMap[k];
				sfx->slot[(e >> 16) & 0xf] = &ch->block[(e >> 24) & 0xf];
				sfx->f98 = ch->n3D == 0 ? ch->base : 0;
			}
		}
		ch->inMap = in;
	}
	return true;
}

// ---------------------------------------------------------------- connection

// SetInputPtr (0x800B19E8): past the fixed count, ignored.
static void setInput(Channel* ch, const int32_t* p)
{
	if (ch->connected >= ch->nFixed) return;
	ch->fixed[ch->connected++] = p ? p : s_zero;
}

// NFSMixMapProc::ConnectMixChannel (0x800B468C): the inputs are master
// channels, then sub channels, then MixCtls and event MixCtls, each read off
// the map until the class changes. One in this state (and map) is this
// copy's; one of another state is added once per copy of it -- through copy
// 0's for the channels, as retail does.
static void connectMixChannel(MixState* s, Channel* ch)
{
	const uint32_t* p = ch->inMap;
	uint32_t id = *p;
	uint32_t cls;
	for (;;) {
		p++;
		cls = id & 0xf0000000u;
		if (cls != 0x20000000u) break;
		const uint32_t st = (id >> 16) & 0xff;
		const Channel* m = stateMaster(id, 0);
		if ((int)st == s->state && (uint32_t)s_map.mapId == ((id >> 4) & 0xf0)) {
			setInput(ch, m ? &m->value : NULL);
		} else {
			for (int c = 0; c < copiesOf(st); c++) setInput(ch, m ? &m->value : NULL);
		}
		id = *p;
	}
	while (cls == 0x30000000u) {
		const uint32_t st = (id >> 16) & 0xff;
		const Channel* m = stateSub(id, 0);
		if ((int)st == s->state && (uint32_t)s_map.mapId == ((id >> 4) & 0xf0)) {
			setInput(ch, m ? &m->value : NULL);
		} else {
			for (int c = 0; c < copiesOf(st); c++) setInput(ch, m ? &m->value : NULL);
		}
		id = *p++;
		cls = id & 0xf0000000u;
	}
	for (;;) {
		if ((cls != 0 && cls != 0xb0000000u) || ch->connected >= ch->nFixed) return;
		const uint32_t st = (id >> 16) & 0xff;
		if (cls == 0xb0000000u) {
			if ((int)st == s->state) {
				const EvtCtl* e = stateEvt(id, 0);
				setInput(ch, e ? &e->out : NULL);
			} else {
				for (int c = 0; c < copiesOf(st); c++) {
					const EvtCtl* e = stateEvt(id, c);
					setInput(ch, e ? &e->out : NULL);
				}
			}
		} else {
			if ((int)st == s->state && (uint32_t)s_map.mapId == ((id >> 8) & 0xf)) {
				const MixCtl* m = stateMixCtl(id, 0);
				setInput(ch, m ? &m->out : NULL);
			} else {
				for (int c = 0; c < copiesOf(st); c++) {
					const MixCtl* m = stateMixCtl(id, c);
					setInput(ch, m ? &m->out : NULL);
				}
			}
		}
		id = *p++;
		cls = id & 0xf0000000u;
	}
}

// GetScaleOutputPtr (0x800B42D8): an SFX's or SFXCTL's output, a 3D control's
// dB, or nUnity.
static const int32_t* scalePtr(MixState* s, uint32_t id)
{
	switch (id & 0xe0000000u) {
	case 0x40000000u:
	case 0x60000000u:
		return &objRead((id & 0xe0ffffffu) | ((uint32_t)s->copy << 11))->out[id & 0xf];
	case 0x80000000u:
		for (int i = 0; i < s->n3D; i++)
			if (s->c3d[i].id == id) return s->c3d[i].out;
		break;
	default:
		break;
	}
	return &s_unity;
}

// ConnectDataProc (0x800B446C): what the data proc reads -- a MixCtl of this
// state, a channel, an object's output or a 3D control.
static void connectDataProc(MixState* s, DataProc* d)
{
	const uint32_t id = d->id;
	switch (id & 0xe0000000u) {
	case 0x00000000u:
		if ((int)(id & 0xff) < s->nCtl) d->in = &s->ctl[id & 0xff].out;
		break;
	case 0x20000000u: {
		const Channel* ch = (id & 0x10000u) ? stateSub(id, 0) : stateMaster(id, 0);
		if (ch) d->in = &ch->value;
		break;
	}
	case 0x40000000u:
	case 0x60000000u:
		d->in = &objRead((id & 0xe0ffffffu) | ((uint32_t)s->copy << 11))->out[id & 0xf];
		break;
	case 0x80000000u: {
		const Ctl3D* c = get3DMixCtl(s, id & 0xf00f00ffu);
		if (c) d->in = c->out;
		break;
	}
	default:
		break;
	}
	// Port guard: retail always finds it.
	if (!d->in) {
		mixLog("%s: data proc %08x reads nothing", kStateName[s->state], (unsigned)id);
		d->in = s_zero;
	}
}

// NFSMixMap::ConnectMapProcs (0x800B2A44), for one state.
static void connectState(MixState* s)
{
	for (int i = 0; i < s->nDp; i++) connectDataProc(s, &s->dp[i]);
	for (int i = 0; i < s->nCtl; i++) s->ctl[i].scale = scalePtr(s, s->ctl[i].scaleId);
	for (int i = 0; i < s->nEvt; i++) {
		EvtCtl* e = &s->evt[i];
		e->scale = scalePtr(s, e->rec[2]);
		e->trig = scalePtr(s, e->rec[3]);
	}
	for (int i = 0; i < s->nSub; i++)
		if (s->sub[i].nFixed > 0) connectMixChannel(s, &s->sub[i]);
	for (int i = 0; i < s->nMaster; i++) connectMixChannel(s, &s->master[i]);
	// Port guard: an input retail would have connected and the port did not.
	// (A master whose SFX is not ported never runs, so its inputs are moot.)
	for (int pass = 0; pass < 2; pass++) {
		Channel* chs = pass ? s->master : s->sub;
		const int n = pass ? s->nMaster : s->nSub;
		for (int i = 0; i < n; i++) {
			Channel* ch = &chs[i];
			if (ch->connected < ch->nFixed && (!pass || ch->block))
				mixLog("%s %s %d: %d of %d inputs connected", kStateName[s->state],
				       pass ? "master" : "sub", i, ch->connected, ch->nFixed);
			for (int k = 0; k < ch->nFixed; k++)
				if (!ch->fixed[k]) ch->fixed[k] = s_zero;
		}
	}
}

// ---------------------------------------------------------------- updates

// NFSMixDataProc::UpdateMixDataProc (0x800B1DD4).
static void dataProcUpdate(DataProc* d)
{
	int32_t v = *d->in > 0 ? *d->in : 0;
	if (v > 0x7fff) v = 0x7fff;
	d->q15 = curveOut(d->curve, v, false);
	d->dB = dbFromQ15(d->q15);
}

// NFSMixCtl::UpdateMixCtl (0x800B1CC8): the data proc's level squeezed into
// the range, in dB, then scaled by a Q15 (or a 3D control's dB).
static void mixCtlUpdate(MixCtl* c)
{
	const int32_t o = dbFromQ15(0x7fff - (((0x7fff - c->dp->q15) * c->ratio) >> 15)) + c->offset;
	int32_t sc = *c->scale;
	if ((c->scaleId & 0xe0000000u) == 0x80000000u) sc = q15FromDb(sc);
	c->out = (o * sc) >> 15;
}

static const float kFrameMs = 0x1.0aaaaep+4f;   // 16.66667 (0x41855557): a 60 Hz frame

static int32_t clampQ15(int32_t v)
{
	return v < 0 ? 0 : (v > 0x7fff ? 0x7fff : v);
}

static void evtShape(EvtCtl* e, uint32_t w, int32_t x)
{
	const int32_t c = curveOut((int)((w >> 12) & 0xf), x, false);
	e->env = e->range < 0 ? c : 0x7fff - c;
}

// UpdateAREvent (0x800B65A0): attack, then release, then done.
static void evtAR(EvtCtl* e)
{
	const uint32_t* r = e->rec;
	const float t = e->timer;
	const float A = (float)(r[4] & 0xfff) * kFrameMs, R = (float)(r[6] & 0xfff) * kFrameMs;
	if (A <= t) {
		if (R <= t - A) { e->env = 0x7fff; e->timer = 0.0f; return; }
		evtShape(e, r[6], clampQ15((int32_t)(32767.0f - ((t - A) * 32767.0f) / R)));
	} else {
		evtShape(e, r[4], (int32_t)((t * 32767.0f) / A));
	}
}

// UpdateASREvent (0x800B6708): attack, hold for the sustain, release.
static void evtASR(EvtCtl* e)
{
	const uint32_t* r = e->rec;
	const float t = e->timer;
	const float A = (float)(r[4] & 0xfff) * kFrameMs, S = (float)(r[5] & 0xfff) * kFrameMs,
	            R = (float)(r[6] & 0xfff) * kFrameMs;
	if (A <= t) {
		if (t - A <= S) { e->env = 0; return; }
		const float u = t - (A + S);
		if (R <= u) { e->env = 0x7fff; e->timer = 0.0f; return; }
		evtShape(e, r[6], clampQ15((int32_t)(32767.0f - (u * 32767.0f) / R)));
	} else {
		evtShape(e, r[4], (int32_t)((t * 32767.0f) / A));
	}
}

// UpdateATREvent (0x800B68B4): attack, hold while the trigger holds, release
// when it lets go; a trigger of 1 again during the release attacks afresh
// from where the output got to.
static void evtATR(EvtCtl* e)
{
	const uint32_t* r = e->rec;
	const float t = e->timer;
	const float A = (float)(r[4] & 0xfff) * kFrameMs, R = (float)(r[6] & 0xfff) * kFrameMs;
	if (A <= t) {
		if (*e->trig != 0) {
			if (*e->trig == 1 && e->mark != 0.0f) {
				e->mark = 0.0f;
				e->active = 1;
				e->start = e->out;
				e->env = 0x7fff;
				e->timer = 0.0f;
				return;
			}
			e->env = 0;
			e->mark = 0.0f;
			e->timer = A;
			e->active = 0;
			e->start = 0;
			return;
		}
		if (e->mark == 0.0f) e->mark = t;
		const float u = e->timer - e->mark;
		if (R < u) {
			e->start = 0;
			e->env = 0x7fff;
			e->mark = 0.0f;
			e->active = 0;
			e->timer = 0.0f;
			return;
		}
		evtShape(e, r[6], (int32_t)(32767.0f - (u * 32767.0f) / R));
	} else {
		if (*e->trig == 0) {
			e->env = 0;
			e->active = 1;
			e->start = e->out;
			e->timer = A;
			return;
		}
		evtShape(e, r[4], (int32_t)((t * 32767.0f) / A));
	}
}

// NFSEvtMixCtl::UpdateEvtMixCtl (0x800B6AB8): the envelope (Q15, 0x7fff =
// none of the range) times the range, scaled by a Q15.
static void evtUpdate(EvtCtl* e, float dt)
{
	if (e->timer == 0.0f && *e->trig == 0) {
		e->active = 0;
		e->mark = 0.0f;
		e->out = 0;
		return;
	}
	e->timer = (float)((double)dt * 1000.0 + (double)e->timer);
	switch ((e->rec[0] >> 24) & 0xf) {
	case 0: evtAR(e); break;
	case 1: evtASR(e); break;
	case 2: evtATR(e); break;
	default: e->out = 0; break;
	}
	const float k = (32767.0f - (float)e->env) * (1.0f / 32768.0f);
	int32_t o;
	if (!e->active) o = (int32_t)(k * (float)e->range);
	else if (*e->trig == 0) o = (int32_t)(k * (float)e->start);
	else o = (int32_t)(k * (float)(e->range - e->start)) + e->start;
	e->out = (int32_t)((float)*e->scale / 32767.0f * (float)o);
}

// NFS3DMixCtl::ChangeCameraState (0x800B5ED4): the record for camera state
// `cam`, else the first.
static void ctl3DChangeCamera(Ctl3D* c, int cam)
{
	const int ncam = (int)((c->hdr >> 24) & 0xf);
	c->cam = c->rec + 1;
	for (int i = 0; i < ncam; i++) {
		const uint32_t* r = c->rec + 1 + i * 6;
		if ((int)((r[0] >> 24) & 0xf) == cam) { c->cam = r; break; }
	}
	ctl3DTellPos(c);
}

// NFS3DMixCtl::Update3DMixCtl (0x800B5FCC): the distance and angle the camera
// record picks (bits 12..15 and 8..11 of its +0), the quadrant of the angle,
// that quadrant's and the next one's distance range ((lo & 0x7fff) .. (hi &
// 0x7fff)) and curves (w1's nibbles 7, 4, 6, 5 for quadrants 0 .. 3), shaped
// into the attenuation; out of reach of both, -100 dB.
static void ctl3DUpdate(Ctl3D* c)
{
	MixObj* o = c->pos;
	if (s_cam.valid && o->out[POS3D_CAMERA] == 0 && s_cam.cur != s_cam.prev)
		ctl3DChangeCamera(c, s_cam.cur);
	const uint32_t w0 = c->cam[0], w1 = c->cam[1];
	const uint32_t distSel = (w0 >> 12) & 0xf, azSel = (w0 >> 8) & 0xf;
	float dist = -1.0f;
	if (distSel == 0) dist = (float)o->out[1];
	else if (distSel == 1) dist = (float)o->out[0];
	if (azSel == 0) c->out[1] = o->out[3];
	else if (azSel == 1) c->out[1] = o->out[2];
	const uint32_t az = (uint32_t)c->out[1];
	static const uint8_t kA[4] = { 2, 3, 4, 5 }, kB[4] = { 3, 4, 5, 2 };
	static const uint8_t kShiftA[4] = { 28, 16, 24, 20 }, kShiftB[4] = { 16, 24, 20, 28 };
	const uint32_t q = (az >> 14) & 3;
	const uint32_t A = c->cam[kA[q]], B = c->cam[kB[q]];
	const int cA = (int)((w1 >> kShiftA[q]) & 0xf), cB = (int)((w1 >> kShiftB[q]) & 0xf);
	const int32_t azq = (int32_t)(az - q * 0x4000);
	const float minA = (float)(A & 0x7fff), maxA = (float)((A >> 16) & 0x7fff);
	const float minB = (float)(B & 0x7fff), maxB = (float)((B >> 16) & 0x7fff);
	if (dist <= maxA || dist <= maxB) {
		float la = dist < minA ? minA : dist;
		float lb = dist < minB ? minB : dist;
		if (maxA < la) la = maxA;
		if (maxB < lb) lb = maxB;
		// Port guard: an empty range divides 0 by 0.
		const int32_t x[2] = {
			maxA > minA ? (int32_t)((la - minA) / (maxA - minA) * 32767.0f) : 0,
			maxB > minB ? (int32_t)((lb - minB) / (maxB - minB) * 32767.0f) : 0,
		};
		c->shaped = azimShape(cA, cB, x, azq);
		c->out[0] = c->shaped;
	} else {
		c->out[0] = -10000;
	}
}

// NFSMixChannel::ProcessMix (0x800B1B20): the base plus every input, clamped.
static void channelMix(Channel* ch)
{
	int32_t v = ch->base;
	for (int k = 0; k < ch->nFixed; k++) v += *ch->fixed[k];
	if (v > ch->max) v = ch->max;
	if (v < ch->min) v = ch->min;
	ch->value = v;
}

// NFSMasterMixChannel::ProcessMix (0x800B1A28): each output word is the
// channel plus its 3D control's dB plus the entry's own offset, as Q15, with
// the 3D control's azimuth (or 0) in the upper half.
static void masterMix(Channel* ch)
{
	static const int32_t kNoAzim = 0;   // r13 - 0x75D8
	if (!ch->block) return;
	channelMix(ch);
	const uint32_t n = ch->outMap[0] & 0xf;
	for (uint32_t k = 1; k <= n; k++) {
		const uint32_t e = ch->outMap[k];
		int32_t* dest = &ch->block[(e >> 24) & 0xf];
		const int32_t* az = &kNoAzim;
		int32_t v;
		if (ch->n3D < 1) {
			v = (int16_t)(e & 0xffff) + ch->value;
		} else {
			const uint32_t i = (e >> 20) & 0xf;
			// Port guard: past the 3D inputs, or one the control lookup missed.
			const int32_t* p = (int)i < ch->n3D && ch->in3D[i] ? ch->in3D[i] : s_zero;
			az = p + 1;
			v = p[0] + (int16_t)(e & 0xffff) + ch->value;
		}
		const uint32_t q = v < -10000 ? 0 : (v < 1 ? (uint32_t)q15FromDb(v) : 0x7fff);
		*dest = (int32_t)(q | ((uint32_t)*az << 16));
	}
}

// NFSMixMapProc::UpdateState (0x800B51E8).
static void updateState(MixState* s, float dt)
{
	for (int i = 0; i < s->nDp; i++) dataProcUpdate(&s->dp[i]);
	for (int i = 0; i < s->nCtl; i++) mixCtlUpdate(&s->ctl[i]);
	for (int i = 0; i < s->nEvt; i++) evtUpdate(&s->evt[i], dt);
	// Port: a control no running channel connected stays as it was built.
	for (int i = 0; i < s->n3D; i++)
		if (s->c3d[i].pos) ctl3DUpdate(&s->c3d[i]);
	for (int i = 0; i < s->nSub; i++) channelMix(&s->sub[i]);
	for (int i = 0; i < s->nMaster; i++) masterMix(&s->master[i]);
}

// ---------------------------------------------------------------- controllers

// EAXSound::GetMasterVolume (0x8007B23C) with its flag set: master times one
// slider (1 speech, 2 game music, 3 menu music, 4 effects, 5 engine, 6 tires).
static int32_t masterVolume(const float v[7], int ch)
{
	static const int8_t kField[7] = { -1, 1, 3, 2, 4, 5, 6 };
	if (ch < 1 || ch > 6) return 0x7fff;
	return (int32_t)(v[0] * v[kField[ch]] * 32767.0f);
}

// SFXCTL_MasterVol::UpdateParams (0x80096CA0) over the AudioSettings block,
// which is what the port's buses hold (applyAudioOptions). Outputs 0..5 are
// how far down each slider has the master's curve, 6 the pause, 8..13 the
// master-scaled sliders. Not ported: 7 (set in one game mode's race, from
// state the port has no counterpart of), 15 (a movie in a range of IDs
// playing), and 14's drag / drift case.
static void busVolumes(float v[7])
{
	v[0] = audioGetBusVolume(AUDIO_BUS_MASTER);
	v[1] = audioGetBusVolume(AUDIO_BUS_SPEECH);
	v[2] = audioGetBusVolume(AUDIO_BUS_FE_MUSIC);
	v[3] = audioGetBusVolume(AUDIO_BUS_IG_MUSIC);
	v[4] = audioGetBusVolume(AUDIO_BUS_SFX);
	v[5] = audioGetBusVolume(AUDIO_BUS_ENGINE);
	v[6] = audioGetBusVolume(AUDIO_BUS_TIRE);
}

static void masterVolUpdate(MixObj* o, const float v[7], bool paused)
{
	const float c = (float)curveOut(1, (int32_t)(v[0] * 32767.0f), false) / 32767.0f;
	static const int8_t kSlider[6] = { 2, 3, 1, 4, 6, 5 };
	for (int i = 0; i < 6; i++)
		o->out[i] = (int32_t)((1.0f - v[0] * v[kSlider[i]] * c) * 32767.0f);
	o->out[6] = paused ? 1 : 0;
	o->out[7] = 0;
	static const int8_t kGmv[6] = { 3, 2, 1, 4, 6, 5 };
	for (int i = 0; i < 6; i++) o->out[8 + i] = masterVolume(v, kGmv[i]);
	o->out[14] = o->out[1];
	o->out[15] = 0;
}

// SFXCTL_Shifting::UpdateMixerOutputs (0x8008FE78): the master-scaled
// sliders again, and the in-game music's scale. Port stand-in: that scale,
// g_IG_Music_Scale, is a music duck the port has not identified; it is taken
// at rest (0x7fff), so output 6 (0x7fff less it) reads 0.
static void shiftingObjUpdate(MixObj* o, const float v[7])
{
	const int32_t musicScale = 0x7fff;
	o->out[0] = masterVolume(v, 1);
	o->out[1] = masterVolume(v, 3);
	o->out[2] = musicScale;
	o->out[3] = masterVolume(v, 4);
	o->out[4] = masterVolume(v, 5);
	o->out[5] = masterVolume(v, 6);
	int32_t d = 0x7fff - musicScale;
	o->out[6] = d < 0 ? 0 : (d > 0x7fff ? 0x7fff : d);
}

// SFXObj_Speech::UpdateParams (0x800A3668): 1 while a line plays. Retail puts
// it on output 0 instead for one value of a speech-module global
// (DAT_803dbb48 == 3) that the port has not identified, so output 0 stays 0.
static void speechObjUpdate(MixObj* o)
{
	if (!speechIsPlaying()) {
		o->out[0] = 0;
		o->out[1] = 0;
	} else {
		o->out[1] = 1;
	}
}

// bNormalize (0x8005D0F4) of a bVector2: a zero vector gives (1, 0).
static void norm2(float out[2], float x, float y)
{
	const float sq = x * x + y * y;
	const float len = sq > 5e-11f ? sqrtf(sq) : 0.0f;
	if (len == 0.0f) { out[0] = 1.0f; out[1] = 0.0f; return; }
	out[0] = x / len;
	out[1] = y / len;
}

// SndCamera::UpdateCameras (0x8008DB18), camera 0: the listener is where the
// camera mover's camera is, and the camera state is 3 while the player is in
// zone 6 or 7 (the crash and jump cameras), else the view's own (1 bumper, 2
// hood, 0 the rest). Its states for an NIS (4) and game state 5 (left as it
// was) are not ported.
static void sndCameraUpdate(const MixMapIn* in)
{
	norm2(s_cam.normCamDir, in->camLook[0] - in->camEye[0], in->camLook[1] - in->camEye[1]);
	s_cam.avgCamDir[0] = s_cam.normCamDir[0];
	s_cam.avgCamDir[1] = s_cam.normCamDir[1];
	s_cam.worldCamPos[0] = in->camEye[0];
	s_cam.worldCamPos[1] = in->camEye[1];
	s_cam.worldCarPos[0] = in->carPos[0];
	s_cam.worldCarPos[1] = in->carPos[1];
	norm2(s_cam.normCarDir, in->carFwd[0], in->carFwd[1]);
	s_cam.carPos[0] = in->carPos[0];
	s_cam.carPos[1] = in->carPos[1];
	s_cam.prev = s_cam.cur;
	if (in->zone == 6 || in->zone == 7)
		s_cam.cur = 3;
	else
		s_cam.cur = in->povType == CARDB_POV_BUMPER ? 1 : (in->povType == CARDB_POV_HOOD ? 2 : 0);
	s_cam.valid = true;
}

// SFXCTL_3DObjPos::SetCameraAngle (0x800973F4): the bAngle of the object off
// `dir` as seen from `pos`, 0 dead ahead, the left half above 0x8000.
static int32_t cameraAngle(const float obj[2], const float dir[2], const float pos[2])
{
	float d[2];
	norm2(d, obj[0] - pos[0], obj[1] - pos[1]);
	uint16_t a = (uint16_t)(0x4000 - bASin(d[0] * dir[0] + d[1] * dir[1]));
	if (-d[1] * dir[0] + d[0] * dir[1] < 0.0f) a = (uint16_t)~a;
	return a;
}

static int32_t dist2(const float a[2], const float b[2])
{
	const float dx = a[0] - b[0], dy = a[1] - b[1];
	const float sq = dx * dx + dy * dy;
	return sq > 5e-11f ? (int32_t)sqrtf(sq) : 0;
}

// SFXCTL_3DObjPos::GenerateSinglePlayerMix (0x800974B4), the single-player
// case of Generate3DParams: 3 and 1 the angle and distance of the object at
// `pos` from the camera, 2 and 0 from the car. The 3DCarPos object is the
// player's car itself; the wheel ones are where SFXCTL_Wheel put them
// (mixmapSetObjPos). Outputs 5, 6 and 10 stay 0: GenerateSinglePlayerMix
// only ever zeroes them (its +0x74 arm, 0x80097954), nothing in the PC or GC
// single-player path sets them.
//
// Then, as UpdateParams (0x80097B10) runs it after the mix, UpdateDoppler
// (0x80097B74; #503): unless the sound is paused, the distance the object
// follows -- mode 1, the constructor's (0x80097294), its distance from the
// player's car (out[0]); mode 0, which only CARSFX_PlayerEngine::InitSFX
// (0x800A0074) sets, on the player's own 3DCarPos, its distance from the
// camera (out[1]) -- becomes a factor 343 / (closing rate + 343), the speed
// of sound over the speed the object comes on at (1 when either distance is
// below 0, as at first), recorded into an Average of 4.
static float s_objDt;
static bool  s_objPaused;
// Debug: the opponents' and traffic's lowest and highest Average since the
// last 5 s report, and whose.
static float s_dopLo = 1.0f, s_dopHi = 1.0f;
static int   s_dopLoObj = -1, s_dopHiObj = -1;

static void dopplerUpdate(MixObj* o)
{
	if (s_objPaused || !(s_objDt > 0.0f)) return;
	o->dopCur = (float)(o == &s_obj[OBJ_3DCARPOS] ? o->out[1] : o->out[0]);
	float f = 1.0f;
	if (o->dopCur >= 0.0f && o->dopPrev >= 0.0f)
		f = 343.0f / ((o->dopCur - o->dopPrev) / s_objDt + 343.0f);
	// Average::Record (0x801A571C) / Recalculate (0x801A5680): the mean of
	// the last four (fewer at first).
	o->dopHist[o->dopI] = f;
	o->dopI = (uint8_t)((o->dopI + 1) % 4);
	if (o->dopN < 4) o->dopN++;
	float sum = 0.0f;
	for (int i = 0; i < o->dopN; i++) sum += o->dopHist[i];
	o->dopAvg = sum / (float)o->dopN;
	o->dopPrev = o->dopCur;
	const int k = (int)(o - s_obj);
	if ((k >= OBJ_AI3DCARPOS && k < OBJ_AI3DCARPOS + MIXMAP_AI_COPIES) ||
	    (k >= OBJ_3DTRAFFICPOS && k < OBJ_3DTRAFFICPOS + MIXMAP_TRAFFIC_COPIES)) {
		if (o->dopAvg < s_dopLo) { s_dopLo = o->dopAvg; s_dopLoObj = k; }
		if (o->dopAvg > s_dopHi) { s_dopHi = o->dopAvg; s_dopHiObj = k; }
	}
}

static void dopplerReset(MixObj* o)
{
	o->dopCur = o->dopPrev = -1.0f;
	memset(o->dopHist, 0, sizeof(o->dopHist));
	o->dopN = o->dopI = 0;
	o->dopAvg = 0.0f;
}

static void objPosUpdate(MixObj* o, const float pos[2])
{
	if (o->noPos) return;
	o->out[3] = cameraAngle(pos, s_cam.avgCamDir, s_cam.worldCamPos);
	o->out[1] = dist2(pos, s_cam.worldCamPos);
	o->out[2] = cameraAngle(pos, s_cam.normCarDir, s_cam.worldCarPos);
	o->out[0] = dist2(pos, s_cam.worldCarPos);
	dopplerUpdate(o);
}

// ---------------------------------------------------------------- public

bool mixmapReady(void) { return s_map.ready; }

void mixmapShutdown(void)
{
	for (int k = 0; k < MIX_STATES * MIX_COPIES; k++) {
		MixState* s = &s_map.st[k / MIX_COPIES][k % MIX_COPIES];
		for (int i = 0; i < s->nSub; i++) free(s->sub[i].fixed);
		for (int i = 0; i < s->nMaster; i++) free(s->master[i].fixed);
		free(s->dp);
		free(s->ctl);
		free(s->evt);
		free(s->c3d);
		free(s->sub);
		free(s->master);
		free(s->outBlock);
	}
	free(s_map.file);
	memset(&s_map, 0, sizeof(s_map));
	memset(&s_cam, 0, sizeof(s_cam));
	for (int i = 0; i < OBJ_COUNT; i++) {
		memset(s_obj[i].out, 0, sizeof(s_obj[i].out));
		memset(s_obj[i].slot, 0, sizeof(s_obj[i].slot));
		s_obj[i].f98 = 0;
		s_obj[i].pos[0] = s_obj[i].pos[1] = 0.0f;
		dopplerReset(&s_obj[i]);
		// The world's 3DObjPos objects start with no position (their
		// constructors' AssignPositionVector(NULL)).
		s_obj[i].noPos = (i >= OBJ_3DFOUNTAINPOS && i < OBJ_PLANE) ||
			(i >= OBJ_3DPLANEPOS && i < OBJ_TRAIN) || (i >= OBJ_3DTRAINPOS && i < OBJ_FRONTEND);
	}
}

// NFSMixMaster::InitMixMap (0x800B57A4): a map state per copy each state's
// manager made -- here the Main and PlayerCar states, one each, and the
// AICar state's five, which is all the single-player game the port runs has
// of them that it sounds -- then InitMainMapStates: every state's channels,
// then their connections. (No other state reads an AICar copy's channels, so
// the five stand idle, and change nothing, when no opponent is racing.)
bool mixmapInit(const char* mxbPath)
{
	mixmapShutdown();
	FILE* f = fopen(mxbPath, "rb");
	if (!f) {
		mixLog("no %s -- the car's sounds keep full volume (copy SOUND/MIXMAPS)", mxbPath);
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	// Four zero words past the end: ConnectMixChannel reads one word past a
	// channel's last input.
	s_map.file = n >= 0x20 && n < (1 << 20) ? calloc((size_t)n + 16, 1) : NULL;
	const bool read = s_map.file && fread(s_map.file, 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	if (!read) {
		mixLog("%s: unreadable", mxbPath);
		mixmapShutdown();
		return false;
	}
	s_map.size = (uint32_t)n;
	const int32_t* hdr = (const int32_t*)s_map.file;
	s_map.mapId = hdr[0];
	if (hdr[1] < 2 || hdr[1] > MIX_STATES || !span(s_map.file + hdr[2], (uint32_t)hdr[1])) {
		mixLog("%s: %d states, table at %#x -- not a mix map", mxbPath, (int)hdr[1], (unsigned)hdr[2]);
		mixmapShutdown();
		return false;
	}
	s_map.copies[0] = 1;   // CSTATEMGR_Main
	s_map.copies[1] = 1;   // CSTATEMGR_PlayerCar, the one human player
	s_map.copies[2] = MIXMAP_AI_COPIES;   // CSTATEMGR_AICar, the opponents
	// CSTATEMGR_TrafficCar, min(the race's traffic drivers, 6). PORT: always
	// six -- a copy no traffic car holds stays at rest and costs a little time.
	s_map.copies[3] = MIXMAP_TRAFFIC_COPIES;
	s_map.copies[5] = MIXMAP_COL_COPIES;  // CSTATEMGR_Collision, five sounds at once
	s_map.copies[6] = MIXMAP_DRIVEBY_COPIES;  // CSTATEMGR_DriveBy, four wooshes at once
	// The world's (#509): CSTATEMGR_Enviro's four fountains (EnterWorld
	// 0x800AB6B0 makes them only for tracks 4000..4198; PORT: always, as the
	// map is made before the track is known -- a copy nothing attaches to
	// stays at rest), CSTATEMGR_Planes' three, CSTATEMGR_Trains' six.
	s_map.copies[4] = MIXMAP_FOUNTAIN_COPIES;
	s_map.copies[7] = MIXMAP_PLANE_COPIES;
	s_map.copies[8] = MIXMAP_TRAIN_COPIES;
	bool ok = true;
	for (int st = 0; ok && st < MIX_STATES; st++)
		for (int c = 0; ok && c < s_map.copies[st]; c++) ok = createState(st, c);
	for (int k = 0; ok && k < MIX_STATES * MIX_COPIES; k++) {
		MixState* s = &s_map.st[k / MIX_COPIES][k % MIX_COPIES];
		if (s->live) ok = initSubChannels(s) && initMasterChannels(s);
	}
	if (!ok) {
		mixLog("%s: malformed -- the car's sounds keep full volume", mxbPath);
		mixmapShutdown();
		return false;
	}
	for (int k = 0; k < MIX_STATES * MIX_COPIES; k++) {
		MixState* s = &s_map.st[k / MIX_COPIES][k % MIX_COPIES];
		if (s->live) connectState(s);
	}
	s_map.ready = true;
	for (int st = 0; st < MIX_STATES; st++) {
		const MixState* s = &s_map.st[st][0];
		if (!s->live) continue;
		if (s_map.copies[st] > 1) mixLog("%s: %d copies", kStateName[st], s_map.copies[st]);
		int run = 0;
		for (int i = 0; i < s->nMaster; i++) run += s->master[i].block != NULL;
		mixLog("%s: %d MixCtls, %d 3D, %d envelopes, %d subs, %d masters (%d with a ported SFX)",
		       kStateName[st], s->nCtl, s->n3D, s->nEvt, s->nSub, s->nMaster, run);
		for (int i = 0; i < s->nMaster; i++) {
			const Channel* ch = &s->master[i];
			if (ch->block)
				mixLog("  master %d -> %s (%08x): %d inputs, %d 3D, base %d", i, ch->sfx->name,
				       (unsigned)ch->outId, ch->nFixed, ch->n3D, (int)ch->base);
		}
	}
	return true;
}

// EAXSound::Update (0x8007BDFC)'s mix part: SndCamera, the controllers, then
// NFSMixMap::ProcessMixMap's states in order.
void mixmapUpdate(const MixMapIn* in, float dt)
{
	if (!s_map.ready) return;
	s_objDt = dt;
	s_objPaused = in->paused;
	sndCameraUpdate(in);
	float v[7];
	busVolumes(v);
	masterVolUpdate(&s_obj[OBJ_MASTERVOL], v, in->paused);
	shiftingObjUpdate(&s_obj[OBJ_SHIFTING], v);
	speechObjUpdate(&s_obj[OBJ_SPEECH]);
	// The 3DObjPos controllers in ID order: 3DCarPos (8), 3DTunnelPos (10),
	// where SFXCTL_Tunnel last saw a tunnel coming or going, then the wheels'
	// 3DRightWheelPos (11) and 3DLeftWheelPos (12).
	objPosUpdate(&s_obj[OBJ_3DCARPOS], s_cam.carPos);
	objPosUpdate(&s_obj[OBJ_3DTUNNELPOS], s_obj[OBJ_3DTUNNELPOS].pos);
	objPosUpdate(&s_obj[OBJ_RWHEELPOS], s_obj[OBJ_RWHEELPOS].pos);
	objPosUpdate(&s_obj[OBJ_LWHEELPOS], s_obj[OBJ_LWHEELPOS].pos);
	// Each opponent's 3DCarPos, where carsfx.c put it (mixmapSetObjPos).
	for (int c = 0; c < MIXMAP_AI_COPIES; c++)
		objPosUpdate(&s_obj[OBJ_AI3DCARPOS + c], s_obj[OBJ_AI3DCARPOS + c].pos);
	// Each Collision copy's 3DColPos and 3DScrapePos, where colsfx.c put them.
	for (int c = 0; c < MIXMAP_COL_COPIES; c++) {
		objPosUpdate(&s_obj[OBJ_3DCOLPOS + c], s_obj[OBJ_3DCOLPOS + c].pos);
		objPosUpdate(&s_obj[OBJ_3DSCRAPEPOS + c], s_obj[OBJ_3DSCRAPEPOS + c].pos);
	}
	// Each DriveBy copy's 3DWooshPos, where carsfx.c put it: its CSTATE's
	// +0x50, the sphere's centre or the passing car.
	for (int c = 0; c < MIXMAP_DRIVEBY_COPIES; c++)
		objPosUpdate(&s_obj[OBJ_3DWOOSHPOS + c], s_obj[OBJ_3DWOOSHPOS + c].pos);
	// Each Traffic copy's 3DTrafficPos (an SFXCTL_3DCarPos), its car's
	// position, where carsfx.c put it.
	for (int c = 0; c < MIXMAP_TRAFFIC_COPIES; c++)
		objPosUpdate(&s_obj[OBJ_3DTRAFFICPOS + c], s_obj[OBJ_3DTRAFFICPOS + c].pos);
	// The world's (#509), each where envsfx.c put it while attached.
	for (int c = 0; c < MIXMAP_FOUNTAIN_COPIES; c++)
		objPosUpdate(&s_obj[OBJ_3DFOUNTAINPOS + c], s_obj[OBJ_3DFOUNTAINPOS + c].pos);
	for (int c = 0; c < MIXMAP_PLANE_COPIES; c++)
		objPosUpdate(&s_obj[OBJ_3DPLANEPOS + c], s_obj[OBJ_3DPLANEPOS + c].pos);
	for (int c = 0; c < MIXMAP_TRAIN_COPIES; c++)
		objPosUpdate(&s_obj[OBJ_3DTRAINPOS + c], s_obj[OBJ_3DTRAINPOS + c].pos);
	for (int k = 0; k < MIX_STATES * MIX_COPIES; k++) {
		MixState* s = &s_map.st[k / MIX_COPIES][k % MIX_COPIES];
		if (s->live) updateState(s, dt);
	}
	if (s_cam.cur != s_cam.prev)
		mixLog("camera state %d -> %d", s_cam.prev, s_cam.cur);
	// Debug: every 5 s, where the nitrous channel stands.
	if ((s_map.reportIn -= dt) <= 0.0f) {
		s_map.reportIn = 5.0f;
		const MixObj* pos = &s_obj[OBJ_3DCARPOS];
		const MixState* car = &s_map.st[1][0];
		const Channel* m = car->nMaster > 4 ? &car->master[4] : NULL;
		const int32_t* p3 = m && m->n3D > 0 && m->in3D[0] ? m->in3D[0] : s_zero;
		mixLog("nitrous: master %d dB/100, 3D %d az %04x; slots %04x/%04x %04x/%04x; "
		       "car %d m %04x, camera %d m %04x; pause %d",
		       m ? (int)m->value : 0, (int)p3[0], (unsigned)(p3[1] & 0xffff),
		       (unsigned)mixmapQ15(MIXMAP_SFX_NITROUS, 0), (unsigned)mixmapAzim(MIXMAP_SFX_NITROUS, 0),
		       (unsigned)mixmapQ15(MIXMAP_SFX_NITROUS, 1), (unsigned)mixmapAzim(MIXMAP_SFX_NITROUS, 1),
		       (int)pos->out[0], (unsigned)pos->out[2], (int)pos->out[1], (unsigned)pos->out[3],
		       (int)s_obj[OBJ_MASTERVOL].out[6]);
		if (s_dopLoObj >= 0 || s_dopHiObj >= 0) {
			mixLog("doppler: lowest %.3f (%s), highest %.3f (%s) in the last 5 s",
			       (double)s_dopLo, s_dopLoObj >= 0 ? s_obj[s_dopLoObj].name : "-",
			       (double)s_dopHi, s_dopHiObj >= 0 ? s_obj[s_dopHiObj].name : "-");
			s_dopLo = s_dopHi = 1.0f;
			s_dopLoObj = s_dopHiObj = -1;
		}
		mixLog("music: slots %04x / %04x, speech %d",
		       (unsigned)mixmapQ15(MIXMAP_SFX_MUSIC, 0), (unsigned)mixmapQ15(MIXMAP_SFX_MUSIC, 1),
		       (int)s_obj[OBJ_SPEECH].out[1]);
	}
}

// SFX_Base::GetQ15Output (0x800BE8B0) / GetAzimOutput (0x800BE8D8). The
// azimuth is the word's upper half (GC's halfword at +0).
int32_t mixmapQ15(uint32_t sfxId, int slot)
{
	if (!s_map.ready) return 0x7fff;
	const MixObj* o = objFind(sfxId);
	if (!o || slot < 0 || slot > 15 || !o->slot[slot]) return 0;
	return *o->slot[slot] & 0x7fff;
}

int32_t mixmapAzim(uint32_t sfxId, int slot)
{
	if (!s_map.ready) return 0;
	const MixObj* o = objFind(sfxId);
	if (!o || slot < 0 || slot > 15 || !o->slot[slot]) return 0;
	return (int32_t)((uint32_t)*o->slot[slot] >> 16);
}

// SFXCTL_3DObjPos::GenDopplerFactor (0x80097C5C): the Average's mean moved
// off 1 by g_DOPPLER_PARAMS[which] (__static_initialization 0x800BE3B4:
// 0.35 the traffic horn, 0.33 the traffic engine, 0.6 the opponents'
// engines, 0.3 the rest), held to 0.8..1.2; 0 while paused (DAT_8031E9B4).
float mixmapDoppler(uint32_t ctlId, int which)
{
	static const float kParams[6] = { 0.35f, 0.33f, 0.6f, 0.3f, 0.3f, 0.3f };
	if (!s_map.ready) return 1.0f;
	const MixObj* o = objFind(ctlId);
	if (!o || which < 0 || which > 5) return 1.0f;
	if (s_objPaused) return 0.0f;
	const float f = (o->dopAvg - 1.0f) * kParams[which] + 1.0f;
	return fminf(fmaxf(f, 0.8f), 1.2f);
}

int32_t* mixmapObjOut(uint32_t id)
{
	MixObj* o = objFind(id);
	return o ? o->out : NULL;
}

void mixmapSetObjPos(uint32_t id, const float pos[3])
{
	MixObj* o = objFind(id);
	if (!o) return;
	o->noPos = !pos;
	if (!pos) return;
	o->pos[0] = pos[0];
	o->pos[1] = pos[1];
}

int32_t mixmapMasterVolume(int ch)
{
	float v[7];
	busVolumes(v);
	return masterVolume(v, ch);
}

int32_t mixmapCurveOut(int t, int32_t x, bool dB) { return curveOut(t, x, dB); }
int32_t mixmapQ15FromDb(int32_t x) { return q15FromDb(x); }
