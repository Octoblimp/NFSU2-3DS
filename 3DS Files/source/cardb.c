#include "cardb.h"
#include "bunidx.h"
#include "carhull.h"
#include "ridephys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See cardb.h for where the table lives and how the engine indexes it.
#define CARDB_CHUNK_TYPE 0x00034600u
// LoaderPerformanceConfigChunk's container and its three sub-chunks, plus the
// Extrema block's own layout -- see cardb_perf_measure() in the header.
#define CARDB_PERF_CHUNK_TYPE 0x80034A30u
#define CARDB_PERF_TABLE_CHUNK 0x00034A31u
#define CARDB_PERF_EXTREMA_CHUNK 0x00034A32u
// The loader's own `if (local_100 < 0x2e)`: past 46 tables it stops reading
// and stops counting, so a longer container cannot shift the mapping.
#define CARDB_PERF_MAX_TABLES 0x2E
#define CARDB_EXTREMA_SIZE 0x78
#define CARDB_EXTREMA_MEASURE_LO 0x28
#define CARDB_EXTREMA_MEASURE_HI 0x2C
#define CARDB_RECORD_SIZE 0x890    // the stride 0x516500 / 0x4E22E0 / 0x511E60 use
#define CARDB_ARRAY_EXTENT 0x189E0 // the literal bound 0x636BE0 and 0x516500 test

// Record field offsets, all confirmed against the shipped PC GLOBALB.BUN.
#define CR_OFF_NAME 0x000
#define CR_OFF_GEOMETRY 0x040
#define CR_OFF_MANUFACTURER 0x0C0
#define CR_OFF_NAMEHASH 0x0D0
#define CR_OFF_FLAGS 0x0DA
#define CR_OFF_WHEEL_OUTER 0x0DC
#define CR_OFF_WHEEL_INNER_MIN 0x0DD
#define CR_OFF_WHEEL_INNER_MAX 0x0DE
#define CR_OFF_CARTYPE 0x840
#define CR_OFF_KIND 0x844
#define CR_OFF_STOCK_PAINT 0x84C
#define CR_OFF_SKIN_COUNT 0x0D9    // s8
#define CR_OFF_TRAFFIC_WEIGHT 0x854
#define CR_OFF_TRAFFIC_NO_LOAD 0x859
#define CR_OFF_SKINS 0x880         // s8 each
#define CR_OFF_BOOST_FLAG 0x0D8    // s8, CarTypeInfo+0xD8; see CarPhysInfo

// The drivetrain block (cardb.h documents how each of these was identified and
// how the set checks itself). CR_OFF_GEARTABLE is the base of the eight-entry
// ratio array whose first two slots are reverse and neutral, so the forward
// gears start two floats in -- which is exactly what lets a signed gear number
// index it directly.
#define CR_OFF_FINALDRIVE 0x2C8
#define CR_OFF_NUMGEARS 0x2D8
#define CR_OFF_GEARTABLE 0x2E0
#define CR_OFF_IDLE_RPM 0x300
#define CR_OFF_REDLINE_RPM 0x304
#define CR_OFF_LIMITER_RPM 0x308
#define CR_OFF_TORQUE_CURVE 0x310
#define CR_OFF_DRAG 0x30C          // per-car aerodynamic drag; see cardb.h

// Rigid-body block -- see cardb.h for the checks that pin these down.
#define CR_OFF_WHEELS 0x120        // CarWheel[4], 0x30 stride, order FL FR RR RL
#define CR_OFF_WHEEL_STRIDE 0x30
#define CR_OFF_WHEEL_POS 0x00      // x forward, y left, z up, from the CG
#define CR_OFF_WHEEL_RADIUS 0x10
#define CR_OFF_WHEEL_TYREWIDTH 0x14
#define CR_OFF_WHEEL_INDEX 0x18    // the record's own 0..3, used to verify the stride
#define CR_OFF_MASS 0x220          // TONNES
#define CR_OFF_BODY_DIMS 0x224     // length, width, height
#define CR_OFF_INERTIA 0x230       // Ixx, then +0x14 Iyy, then +0x14 Izz, tonne m^2
#define CR_OFF_INERTIA_STRIDE 0x14
#define CR_OFF_CAMERA_INFO 0x720   // CameraInfo: count, then POVs from +0x10
#define CR_OFF_HUD_INFO 0x7F0      // CarHudInfo; see CarRec.hudAngle
#define CR_OFF_POV_STRIDE 0x10

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// The record's floats are little-endian IEEE-754, same as the ARM11's, so this
// is a reinterpret rather than a conversion. Goes through memcpy because the
// record buffer has no alignment guarantee and a cast would be a misaligned
// load on ARM.
static float rdf32le(const u8* p) {
	u32 bits = rd32le(p);
	float out;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

// Fills `out` with the rigid body. Refuses anything that fails the internal
// checks cardb.h describes rather than handing carsim a car with no mass: the
// four wheels have to carry their own stored indices in order, the body box has
// to be a real size, and the stored inertia has to be the uniform-box tensor of
// the stored mass and dimensions. That last test is the strong one -- it ties
// four separate offsets together, and a misread of any of them fails it.
static void parseChassis(const u8* rec, CarChassis* out) {
	memset(out, 0, sizeof(*out));

	out->mass = rdf32le(rec + CR_OFF_MASS) * 1000.0f;   // tonnes -> kg
	out->length = rdf32le(rec + CR_OFF_BODY_DIMS + 0);
	out->width  = rdf32le(rec + CR_OFF_BODY_DIMS + 4);
	out->height = rdf32le(rec + CR_OFF_BODY_DIMS + 8);
	for (int i = 0; i < 3; i++)
		out->inertia[i] = rdf32le(rec + CR_OFF_INERTIA + i * CR_OFF_INERTIA_STRIDE) * 1000.0f;

	// Validated separately from `ok`: a record whose drag reads as nonsense is
	// still a perfectly good rigid body, and carsim.c would rather substitute a
	// drag than throw away a real mass and a real wheelbase over it.
	out->dragCoef = rdf32le(rec + CR_OFF_DRAG);
	if (!(out->dragCoef > 1.0e-6f && out->dragCoef < 1.0e-2f))
		out->dragCoef = 0.0f;

	bool ok = out->mass > 200.0f && out->mass < 40000.0f &&
		out->length > 1.0f && out->length < 25.0f &&
		out->width > 0.5f && out->width < 5.0f &&
		out->height > 0.5f && out->height < 6.0f;

	for (int i = 0; i < CARPHYS_WHEELS; i++) {
		const u8* w = rec + CR_OFF_WHEELS + i * CR_OFF_WHEEL_STRIDE;
		CarWheel* d = &out->wheel[i];
		for (int c = 0; c < 3; c++)
			d->pos[c] = rdf32le(w + CR_OFF_WHEEL_POS + c * 4);
		d->radius = rdf32le(w + CR_OFF_WHEEL_RADIUS);
		d->tyreWidth = rdf32le(w + CR_OFF_WHEEL_TYREWIDTH);
		// The wheel's own index, which is what makes the 0x30 stride certain.
		if ((int)rd32le(w + CR_OFF_WHEEL_INDEX) != i)
			ok = false;
		if (d->radius < 0.15f || d->radius > 1.2f)
			ok = false;
	}

	if (ok) {
		// The record's y is positive to the LEFT. Order is FL, FR, RR, RL.
		out->cgToFront = out->wheel[0].pos[0];
		out->cgToRear = -out->wheel[2].pos[0];
		out->wheelbase = out->cgToFront + out->cgToRear;
		out->trackFront = out->wheel[0].pos[1] - out->wheel[1].pos[1];
		out->trackRear = out->wheel[3].pos[1] - out->wheel[2].pos[1];
		if (out->trackFront < 0.0f) out->trackFront = -out->trackFront;
		if (out->trackRear < 0.0f) out->trackRear = -out->trackRear;
		out->wheelRadius = 0.25f * (out->wheel[0].radius + out->wheel[1].radius +
			out->wheel[2].radius + out->wheel[3].radius);
		if (out->cgToFront <= 0.1f || out->cgToRear <= 0.1f ||
			out->trackFront <= 0.3f || out->trackRear <= 0.3f)
			ok = false;
	}

	// The tie-breaker: inertia must be the uniform box of the mass and dims.
	if (ok) {
		float L = out->length, W = out->width, H = out->height, m = out->mass;
		float pred[3] = {
			m * (W * W + H * H) / 12.0f,
			m * (L * L + H * H) / 12.0f,
			m * (L * L + W * W) / 12.0f
		};
		for (int i = 0; i < 3; i++) {
			float d = out->inertia[i] - pred[i];
			if (d < 0.0f) d = -d;
			if (d > pred[i] * 0.02f)
				ok = false;
		}
	}

	out->valid = ok;
}

// Fills `out` from one raw record. A record with no engine in it -- the
// traffic entries carry zeros -- comes back with valid = false rather than a
// gearbox that would divide by zero the first time it was asked for an rpm.
static void parsePerf(const u8* rec, CarPerf* out) {
	memset(out, 0, sizeof(*out));
	out->finalDrive = rdf32le(rec + CR_OFF_FINALDRIVE);
	out->numGears = (int)rd32le(rec + CR_OFF_NUMGEARS);
	out->reverseRatio = rdf32le(rec + CR_OFF_GEARTABLE);
	out->idleRpm = rdf32le(rec + CR_OFF_IDLE_RPM);
	out->redlineRpm = rdf32le(rec + CR_OFF_REDLINE_RPM);
	out->limiterRpm = rdf32le(rec + CR_OFF_LIMITER_RPM);
	if (out->numGears < 1 || out->numGears > CARPERF_MAX_GEARS)
		out->numGears = 0;
	// Forward gears start after the reverse and neutral slots.
	for (int g = 0; g < out->numGears; g++)
		out->gearRatio[g] = rdf32le(rec + CR_OFF_GEARTABLE + 8 + g * 4);
	for (int i = 0; i < CARPERF_TORQUE_POINTS; i++)
		out->torque[i] = rdf32le(rec + CR_OFF_TORQUE_CURVE + i * 4);

	// The consistency check cardb.h describes, applied per record rather than
	// trusted from the offline scan: every forward ratio the count claims has
	// to actually be there and descend, the final drive has to be a real one,
	// and the rev range has to be ordered.
	bool ok = out->numGears > 0 && out->finalDrive > 1.0f && out->finalDrive < 10.0f &&
		out->idleRpm > 0.0f && out->redlineRpm > out->idleRpm &&
		out->limiterRpm >= out->redlineRpm && out->reverseRatio != 0.0f;
	for (int g = 0; ok && g < out->numGears; g++) {
		if (out->gearRatio[g] <= 0.0f)
			ok = false;
		else if (g > 0 && out->gearRatio[g] >= out->gearRatio[g - 1])
			ok = false;
	}
	bool anyTorque = false;
	for (int i = 0; i < CARPERF_TORQUE_POINTS; i++)
		if (out->torque[i] > 0.0f)
			anyTorque = true;
	out->valid = ok && anyTorque;
}

float carperf_torque_at(const CarPerf* p, float rpm) {
	if (!p || !p->valid)
		return 0.0f;
	// The nine points are sampled evenly across idle..limiter.
	float span = p->limiterRpm - p->idleRpm;
	if (span <= 0.0f)
		return 0.0f;
	float t = (rpm - p->idleRpm) / span * (float)(CARPERF_TORQUE_POINTS - 1);
	if (t <= 0.0f)
		return p->torque[0];
	if (t >= (float)(CARPERF_TORQUE_POINTS - 1))
		return p->torque[CARPERF_TORQUE_POINTS - 1];
	int i = (int)t;
	float f = t - (float)i;
	return p->torque[i] + (p->torque[i + 1] - p->torque[i]) * f;
}

// Total reduction from engine to road wheel for `gear`, or 0 in neutral. The
// reverse slot is already negative in the table, so the sign falls out of the
// data and the caller never special-cases it.
static float gearTotalRatio(const CarPerf* p, int gear) {
	if (!p || !p->valid)
		return 0.0f;
	if (gear < 0)
		return p->reverseRatio * p->finalDrive;
	if (gear == 0 || gear > p->numGears)
		return 0.0f;
	return p->gearRatio[gear - 1] * p->finalDrive;
}

float carperf_speed_at_rpm(const CarPerf* p, int gear, float rpm, float wheelRadius) {
	float ratio = gearTotalRatio(p, gear);
	if (ratio == 0.0f)
		return 0.0f;
	// rpm -> rad/s is 2*pi/60; the wheel then turns `ratio` times slower.
	return rpm * 0.104719755f / ratio * wheelRadius;
}

float carperf_rpm_at_speed(const CarPerf* p, int gear, float speed, float wheelRadius) {
	float ratio = gearTotalRatio(p, gear);
	if (ratio == 0.0f || wheelRadius <= 0.0f)
		return p ? p->idleRpm : 0.0f;
	float rpm = speed / wheelRadius * ratio / 0.104719755f;
	return rpm < 0.0f ? -rpm : rpm;
}

// SPEED2.EXE:0x505450, and inlined into the sprintf-and-hash wrapper 0x50B9C0
// that 0x511E60 actually calls: seed -1, fold a..z to A..Z, h = c + h * 0x21.
// This is the same djb2 variant fng.c/trackdb.c already port; the difference is
// the uppercase fold, which matters here because the record strings are the
// hash input rather than a literal typed into the exe. (Every name and
// manufacturer a Quick Race row can reach is already uppercase in the shipped
// table -- only the traffic records carry mixed case -- so the fold is provably
// a no-op on this screen. It is transcribed anyway because the function's
// behaviour, not this table's contents, is what is being ported.)
static u32 feHashUpper(const char* s) {
	u32 h = 0xFFFFFFFFu;
	for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
		unsigned char c = *p;
		if (c >= 'a' && c <= 'z')
			c = (unsigned char)(c - 0x20);
		h = (u32)c + h * 0x21u;
	}
	return h;
}

// The two format strings 0x511E60 passes to 0x50B9C0, at .rdata 0x79ADA8 and
// 0x79ADE0. Composed here rather than sprintf'd through a format because the
// only conversion either carries is a single trailing %s.
#define CARDB_BADGE_PREFIX "SECONDARY_LOGO_"        // .rdata 0x79ADA8
#define CARDB_LOGO_PREFIX "CARSELECT_MANUFACTURER_" // .rdata 0x79ADE0

static u32 hashPrefixed(const char* prefix, const char* name) {
	char key[80];
	snprintf(key, sizeof(key), "%s%s", prefix, name);
	return feHashUpper(key);
}

// LoaderPerformanceConfigChunk (Speed.elf 0x801C3688) over one already-found
// 0x80034A30 container: its 0x34A31 sub-chunks in file order, one per car
// type, and its one 0x34A32 Extrema block. `begin`/`end` span the container's
// payload in the open file. Anything missing simply leaves measureOk false.
static void readPerfConfig(FILE* f, long begin, long end, CarDb* out) {
	long co = begin;
	int nth = 0;
	while (co + 8 <= end) {
		u8 h[8];
		if (fseek(f, co, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8)
			break;
		const u32 ct = rd32le(h);
		const u32 cs = rd32le(h + 4);
		const long pay = co + 8;
		if (ct == CARDB_PERF_TABLE_CHUNK) {
			u8 cb[4];
			if (fseek(f, pay, SEEK_SET) == 0 && fread(cb, 1, 4, f) == 4) {
				const u32 rows = rd32le(cb);
				if (rows > 0 && nth < CARDB_PERF_MAX_TABLES) {
					u8 v0[4], vN[4];
					const long r0 = pay + 4;
					const long rN = r0 + (long)(rows - 1) * CARDB_PERF_ROW_SIZE;
					if (rN + 4 <= pay + (long)cs &&
							fseek(f, r0, SEEK_SET) == 0 && fread(v0, 1, 4, f) == 4 &&
							fseek(f, rN, SEEK_SET) == 0 && fread(vN, 1, 4, f) == 4) {
						for (int i = 0; i < out->count; i++) {
							if (out->recs[i].carType != nth)
								continue;
							out->recs[i].measureStock = rdf32le(v0);
							out->recs[i].measureMax = rdf32le(vN);
							// A traffic record's table is one empty row; the
							// engine never asks for its measure, and a zero
							// would read as the slowest car in the game.
							out->recs[i].measureValid = rows > 1;
							out->recs[i].perfRowsAt = r0;
							out->recs[i].perfRowCount = (int)rows;
							break;
						}
					}
				}
				if (nth < CARDB_PERF_MAX_TABLES)
					nth++;
			}
		} else if (ct == CARDB_PERF_EXTREMA_CHUNK && cs >= CARDB_EXTREMA_SIZE) {
			u8 ex[CARDB_EXTREMA_SIZE];
			if (fseek(f, pay, SEEK_SET) == 0 &&
					fread(ex, 1, CARDB_EXTREMA_SIZE, f) == CARDB_EXTREMA_SIZE) {
				out->measureLo = rdf32le(ex + CARDB_EXTREMA_MEASURE_LO);
				out->measureHi = rdf32le(ex + CARDB_EXTREMA_MEASURE_HI);
				out->measureOk = out->measureHi > out->measureLo;
				for (int k = 0; k < CARDB_EXTREMA_FLOATS; k++)
					out->extrema[k] = rdf32le(ex + 4 * k);
				out->extremaOk = true;
			}
		}
		if (cs == 0 && ct == 0)
			break;
		co = pay + (long)cs;
	}
}

bool cardb_load(const char* bunPath, CarDb* out) {
	memset(out, 0, sizeof(*out));

	FILE* f = fopen(bunPath, "rb");
	if (!f)
		return false;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long fileSize = ftell(f);
	if (fileSize <= 0) {
		fclose(f);
		return false;
	}

	// 0x34600 is a top-level chunk in GLOBALB.BUN (verified: file offset
	// 7101696); the shared index (bunidx.h) says where without a header read
	// per chunk from the start of the file.
	long chunkOff = -1;
	u32 chunkSize = 0;
	if (!bunidx_find(bunPath, CARDB_CHUNK_TYPE, &chunkOff, &chunkSize))
		chunkOff = -1;

	if (chunkOff < 0) {
		fclose(f);
		return false;
	}

	// SPEED2.EXE:0x636BE0's own base rule, verbatim: the record array starts at
	// (chunk + 0x17) & ~0xF -- the 8-byte header skipped and the result rounded
	// up to 16. The engine applies it to the loaded chunk's address; applying it
	// to the file offset gives the same answer because every top-level chunk in
	// a .BUN is itself 16-byte aligned (7101696 is), which is the property the
	// whole container relies on to keep DXT blocks aligned.
	long base = (chunkOff + 0x17) & ~0xFL;
	out->recBase = base;

	// The engine's count is the literal extent 0x189E0 at a 0x890 stride: 46.
	// It is not derived from the chunk header, so it is transcribed as the
	// constant it is -- but clamped to what the chunk actually holds, because a
	// short or absent chunk in some other SKU's data would otherwise read past
	// the payload.
	long avail = (chunkOff + 8 + (long)chunkSize) - base;
	long extent = CARDB_ARRAY_EXTENT;
	if (avail < extent)
		extent = avail;
	if (extent < CARDB_RECORD_SIZE) {
		fclose(f);
		return false;
	}
	int count = (int)(extent / CARDB_RECORD_SIZE);
	if (count > CARDB_MAX_RECORDS)
		count = CARDB_MAX_RECORDS;

	// Only the seven fields the frontend reads are kept, so the resident cost
	// is count * ~80 bytes rather than count * 2192.
	// The records come in one read (46 x 2192 bytes) rather than a seek and a
	// read each (#270).
	u8* all = (u8*)malloc((size_t)count * CARDB_RECORD_SIZE);
	if (!all) {
		fclose(f);
		return false;
	}
	if (fseek(f, base, SEEK_SET) != 0)
		count = 0;
	else
		count = (int)fread(all, CARDB_RECORD_SIZE, (size_t)count, f);
	// Each car's collision hull rides along with its physics block, by name
	// (the CarCollisionBody ctor's GetDefaultCollisionVolumeNameHash).
	carhull_load(bunPath);
	int kept = 0;
	for (int i = 0; i < count; i++) {
		const u8* rec = all + (size_t)i * CARDB_RECORD_SIZE;
		CarRec* r = &out->recs[kept++];
		memcpy(r->name, rec + CR_OFF_NAME, 31);
		r->name[31] = '\0';
		memcpy(r->manufacturer, rec + CR_OFF_MANUFACTURER, 31);
		r->manufacturer[31] = '\0';
		r->nameHash = rd32le(rec + CR_OFF_NAMEHASH);
		r->hasGeometry = (rec[CR_OFF_GEOMETRY] != 0) ? 1 : 0;
		// Directory component of "CARS\<dir>\GEOMETRY.BIN" -- the text between
		// the first and the last separator. Accepts either separator so the
		// parse does not depend on the shipped path being Windows-flavoured,
		// and yields "" for anything that is not a two-separator path, which
		// the caller treats the same as "no model on the SD card".
		r->geomDir[0] = '\0';
		if (r->hasGeometry) {
			const char* path = (const char*)(rec + CR_OFF_GEOMETRY);
			const char* first = NULL;
			const char* last = NULL;
			for (const char* p = path; *p && (p - path) < 32; p++) {
				if (*p != '\\' && *p != '/')
					continue;
				if (!first)
					first = p;
				last = p;
			}
			if (first && last && last > first + 1) {
				size_t n = (size_t)(last - first - 1);
				if (n > sizeof(r->geomDir) - 1)
					n = sizeof(r->geomDir) - 1;
				memcpy(r->geomDir, first + 1, n);
				r->geomDir[n] = '\0';
			}
		}
		r->flags = rec[CR_OFF_FLAGS];
		r->wheelOuter = (s8)rec[CR_OFF_WHEEL_OUTER];
		r->wheelInnerMin = (s8)rec[CR_OFF_WHEEL_INNER_MIN];
		r->wheelInnerMax = (s8)rec[CR_OFF_WHEEL_INNER_MAX];
		r->carType = (int)rd32le(rec + CR_OFF_CARTYPE);
		r->kind = (int)rd32le(rec + CR_OFF_KIND);
		r->stockPaint = rd32le(rec + CR_OFF_STOCK_PAINT);
		memcpy(r->trafficWeight, rec + CR_OFF_TRAFFIC_WEIGHT, CARDB_TRAFFIC_ZONE_TYPES);
		memcpy(r->trafficNoLoad, rec + CR_OFF_TRAFFIC_NO_LOAD, CARDB_TRAFFIC_ZONE_TYPES);
		r->skinCount = (s8)rec[CR_OFF_SKIN_COUNT];
		memcpy(r->skins, rec + CR_OFF_SKINS, CARDB_MAX_SKINS);
		parsePerf(rec, &r->perf);
		parseChassis(rec, &r->chassis);
		memcpy(r->phys.raw, rec + CARPHYS_CPI_OFFSET, CARPHYS_CPI_SIZE);
		r->phys.boostFlag = (s8)rec[CR_OFF_BOOST_FLAG];
		// The block is only as good as the two parses that already vet its
		// wheel array, mass, body box and drivetrain.
		r->phys.valid = r->perf.valid && r->chassis.valid;
		r->phys.hullN = carhull_find(r->name, &r->phys.hull);
		for (int k = 0; k < 4; k++)
			r->hudAngle[k] = rdf32le(rec + CR_OFF_HUD_INFO + 4 + 4 * k);
		const u8* ci = rec + CR_OFF_CAMERA_INFO;
		int nPov = (int)rd32le(ci);
		if (nPov < 0) nPov = 0;
		if (nPov > CARDB_POV_MAX) nPov = CARDB_POV_MAX;
		r->povCount = nPov;
		for (int k = 0; k < nPov; k++) {
			const u8* pv = ci + 0x10 + k * CR_OFF_POV_STRIDE;
			r->pov[k].type = (s16)(pv[0] | (pv[1] << 8));
			r->pov[k].angle = (u16)(pv[2] | (pv[3] << 8));
			r->pov[k].eye[0] = rdf32le(pv + 4);
			r->pov[k].eye[1] = rdf32le(pv + 12);
			r->pov[k].eye[2] = rdf32le(pv + 8);
		}
	}
	out->count = kept;

	// The performance-config container, found the same way: it is a top-level
	// chunk in GLOBALB.BUN too (PC file offset 7770112). See
	// cardb_perf_measure() in the header for the layout and where it is from.
	long po = 0;
	u32 sz = 0;
	if (bunidx_find(bunPath, CARDB_PERF_CHUNK_TYPE, &po, &sz))
		readPerfConfig(f, po + 8, po + 8 + (long)sz, out);

	// Every player car's stock build while the whole records are in hand:
	// RebuildPhysicsInfo with nothing fitted, which needs only the record. It
	// replaces the shipped block in `phys`, because the block retail drives is
	// the built one -- Car's ctor (Speed.elf 0x8021DB60) and ChangeCarType
	// (0x8021E89C) take RidePhysicsInfo::GetCarPhysicsInfo -- and the build is
	// not a no-op on a stock car: BuildCarPhysicsInfo folds the stock boost
	// curve into the torque curve every time (the seven turbo cars' torque
	// rises by up to half), and BuildInertiaTensor recomputes the tensor
	// (float rounding only). Engine::GetTorque (0x801B35D4) reads that torque
	// table and nothing else; the turbo pressure is the gauge's and the
	// sound's. TASKS.md #288. With Extrema the build also gives the three
	// ratings. The traffic records (kind != 0) keep their shipped block.
	for (int i = 0; i < kept; i++) {
		CarRec* r = &out->recs[i];
		if (r->kind != 0 || !r->phys.valid)
			continue;
		RidePhys rp;
		ridephys_build(&rp, all + (size_t)i * CARDB_RECORD_SIZE + CARPHYS_CPI_OFFSET,
			NULL, NULL, NULL);
		memcpy(r->phys.raw, rp.cpi, CARPHYS_CPI_SIZE);
		if (out->extremaOk) {
			ridephys_ratings(&rp, out->extrema, r->rating);
			r->ratingValid = true;
		}
	}
	free(all);

	fclose(f);
	return kept > 0;
}

bool cardb_read_compleat(const char* bunPath, const CarDb* db, int carType, u8* out) {
	if (!db || !out || db->recBase <= 0)
		return false;
	int idx = -1;
	for (int i = 0; i < db->count; i++) {
		if (db->recs[i].carType == carType) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		return false;
	FILE* f = fopen(bunPath, "rb");
	if (!f)
		return false;
	const long at = db->recBase + (long)idx * CARDB_RECORD_SIZE + CARPHYS_CPI_OFFSET;
	const bool ok = fseek(f, at, SEEK_SET) == 0 &&
		fread(out, 1, RIDEPHYS_COMPLEAT_SIZE, f) == RIDEPHYS_COMPLEAT_SIZE;
	fclose(f);
	return ok;
}

int cardb_read_perf_rows(const char* bunPath, const CarDb* db, int carType, u8* rows,
		int maxRows) {
	if (!db || !rows || maxRows <= 0)
		return 0;
	const CarRec* r = NULL;
	for (int i = 0; i < db->count; i++) {
		if (db->recs[i].carType == carType) {
			r = &db->recs[i];
			break;
		}
	}
	if (!r || r->perfRowsAt <= 0 || r->perfRowCount <= 0)
		return 0;
	const int n = r->perfRowCount < maxRows ? r->perfRowCount : maxRows;
	FILE* f = fopen(bunPath, "rb");
	if (!f)
		return 0;
	const size_t want = (size_t)n * CARDB_PERF_ROW_SIZE;
	const bool ok = fseek(f, r->perfRowsAt, SEEK_SET) == 0 && fread(rows, 1, want, f) == want;
	fclose(f);
	return ok ? n : 0;
}

float cardb_perf_measure(const CarDb* db, int carType) {
	if (!db || !db->measureOk)
		return -1.0f;
	const float span = db->measureHi - db->measureLo;
	if (span <= 0.0f)
		return -1.0f;
	for (int i = 0; i < db->count; i++) {
		if (db->recs[i].carType != carType)
			continue;
		if (!db->recs[i].measureValid)
			return -1.0f;
		return (db->recs[i].measureStock - db->measureLo) / span * CARDB_MEASURE_SCALE;
	}
	return -1.0f;
}

// SPEED2.EXE:0x570A30 -- `return [0x864F24]`. That global lives in BSS and has
// only read xrefs across the whole exe (no code writes it, no data section
// initialises it), so on the PC retail build the region is always 0, the US
// build. The other regions' branches are transcribed because a PAL/JP disc's
// own build takes them.
//
// PORT (#415): the user asked for a region choice, the config's Region key
// (cardb_set_build_region). 0 stays the default, retail PC's. What a region
// changes, from SPEED2.EXE's readers of [0x864F24] (tools/re/wip415):
//   - 0x5022C0: the cars not shipped there. Region 0 drops CORSA and PEUGOT106,
//     the others the types 5 and 18 (cardb_region_allows).
//   - 0x511E60: the badges and logos (cardb_logo_hash), e.g. the Corsa as a
//     Vauxhall in Europe and the G35 as a Skyline in Japan.
//   - Opponents' random cars: the North American or Rest of World list
//     (opponent.c pickRandomCar).
//   - 0x5712A0 / 0x571380: the language and country codes (US/EN for 0; a
//     European one from the language for 1 and 5..9; KR 2 and 10, JP 3, TW 4,
//     CN 11, TH 12). Not ported; nothing here reads them.
//   - 0x57FB40: GlobalTexturesNA.bin or ...ROW.bin, which differ only in
//     ADS_BESTBUY_BBH_A. Not ported: the port loads neither.
//   - 0x575770: the needperformance1 table's AD_BESTBUY entries off outside
//     region 0. Not ported (no such table here).
static int s_buildRegion = 0;

int cardb_build_region(void) {
	return s_buildRegion;
}

void cardb_set_build_region(int region) {
	s_buildRegion = (region >= 0 && region <= CARDB_REGION_MAX) || region == CARDB_REGION_ALL
		? region : 0;
}

// SPEED2.EXE:0x570A40 -- "is this one of the European regions", true for region
// 1 and regions 5..9. Reads the same global as cardb_build_region().
static bool cardb_region_is_euro(int region) {
	return region == 1 || (region >= 5 && region <= 9);
}

// [0x7F70D0], the sub-region flag 0x511E60 tests inside its European arms to
// pick Vauxhall/Corsa (UK) over Opel/Opel_Corsa. Unreachable from region 0 and
// not otherwise decoded, so it is named and defaulted rather than guessed at;
// the branch it gates is transcribed in full either way.
#define CARDB_EURO_SUBREGION_UK 0

// SPEED2.EXE:0x5022C0 -- the per-region "not shipped here" list. Both tables
// are two ints long and sit adjacent in .data: 0x7F7C48 = {7, 6} for region 0,
// 0x7F7C50 = {5, 18} for every other region.
//
// On the shipped US data this is what takes CORSA (7) and PEUGOT106 (6) out of
// the Quick Race list, leaving 29 of the 31 stock cars.
//
// The engine does short-circuit this whole test to "available" when its global
// 0x838464 is set, and that arm is transcribed below -- but 0x838464 is the
// debug/cheat unlock-all global, which no shipped path sets, and this port's
// callers pass false. See cardb_build_rows() for why that global must not be
// tied to the unlockAll this module's row builder takes: they short-circuit two
// different gates for two unrelated reasons, and conflating them puts two cars
// on the screen that the US disc does not ship.
static const int kBlockedRegion0[2] = { 7, 6 }; // .data 0x7F7C48
static const int kBlockedOther[2] = { 5, 18 };  // .data 0x7F7C50

int cardb_region_for_car(int region, int carType) {
	if (region != CARDB_REGION_ALL)
		return region;
	for (int i = 0; i < 2; i++)
		if (kBlockedRegion0[i] == carType)
			return 1;
	return 0;
}

bool cardb_region_allows(int carType, int buildRegion, bool debugUnlockAll) {
	if (debugUnlockAll || buildRegion == CARDB_REGION_ALL)
		return true;
	const int* blocked = (buildRegion != 0) ? kBlockedOther : kBlockedRegion0;
	for (int i = 0; i < 2; i++) {
		if (blocked[i] == carType)
			return false;
	}
	return true;
}

// SPEED2.EXE:0x511E60, transcribed arm for arm. Both halves start from the
// record's own string and only substitute when a region override names another
// one; the substituted names are the .rdata literals at 0x79AD2C..0x79ADD4.
u32 cardb_logo_hash(const CarRec* r, int buildRegion, bool manufacturer) {
	if (!r)
		return 0;
	buildRegion = cardb_region_for_car(buildRegion, r->carType);

	if (manufacturer) {
		// 0x610170 is `carInfo + 0xC0`, i.e. the record's manufacturer string.
		u32 h = hashPrefixed(CARDB_LOGO_PREFIX, r->manufacturer);
		if (buildRegion == 0)
			return h;
		if (!cardb_region_is_euro(buildRegion)) {
			if (buildRegion == 3) {
				// JP: the G35 is sold as a Nissan Skyline, the IS300 as a
				// Toyota Altezza.
				if (r->carType == 0x17)
					return hashPrefixed(CARDB_LOGO_PREFIX, "NISSAN");
				if (r->carType == 0x0E)
					return hashPrefixed(CARDB_LOGO_PREFIX, "TOYOTA");
			}
			if (r->carType != 7)
				return h;
		} else {
			if (r->carType != 7)
				return h;
			if (CARDB_EURO_SUBREGION_UK == 0)
				return hashPrefixed(CARDB_LOGO_PREFIX, "VAUXHALL");
		}
		return hashPrefixed(CARDB_LOGO_PREFIX, "OPEL");
	}

	u32 h = hashPrefixed(CARDB_BADGE_PREFIX, r->name);
	const char* sub;
	if (buildRegion == 0) {
		// Region 0 re-states two names the records already carry (MIATA,
		// TIBURON), so this arm is a literal no-op on the shipped table --
		// transcribed rather than skipped, so the shape of the function
		// survives into the port.
		if (r->carType == 0x04)
			sub = "MIATA";
		else if (r->carType == 0x0B)
			sub = "TIBURON";
		else
			return h;
	} else if (!cardb_region_is_euro(buildRegion)) {
		if (buildRegion == 2) {
			if (r->carType == 0x04)
				sub = "MX5";
			else if (r->carType == 0x0B)
				sub = "TUSCANI";
			else if (r->carType == 0x07)
				sub = "OPEL_CORSA";
			else
				return h;
		} else if (buildRegion == 3) {
			switch (r->carType) {
			case 0x04: sub = "MIATA_ROADSTER_JAPAN"; break;
			case 0x02: sub = "TRUENO"; break;
			case 0x0E: sub = "ALTEZZA"; break;
			case 0x0B: sub = "COUPE"; break;
			case 0x16: sub = "FAIRLADYZ"; break;
			case 0x17: sub = "SKYLINE_JAPAN"; break;
			case 0x07: sub = "OPEL_CORSA"; break;
			default: return h;
			}
		} else {
			if (r->carType == 0x04)
				sub = "MX5";
			else if (r->carType == 0x0B)
				sub = "COUPE";
			else if (r->carType == 0x07)
				sub = "OPEL_CORSA";
			else
				return h;
		}
	} else {
		if (r->carType == 0x04)
			sub = "MX5";
		else if (r->carType == 0x0B)
			sub = "COUPE";
		else if (r->carType == 0x07)
			sub = (CARDB_EURO_SUBREGION_UK == 0) ? "CORSA" : "OPEL_CORSA";
		else
			return h;
	}
	return hashPrefixed(CARDB_BADGE_PREFIX, sub);
}

// The fallback 0x4B2310 installs when 0x4901D0 says the computed hash names no
// texture in the loaded packs. It is a real 256x64 entry in
// Global\FrontEndTextures.tpk (hash C8E80D1F), not a hole.
u32 cardb_placeholder_hash(void) {
	return feHashUpper("GENERICPLACEHOLDER");
}

u32 cardb_name_label_hash(const CarRec* r) {
	return hashPrefixed("CAR_NAME_", r->name);
}

// SPEED2.EXE:0x497DA0 -- a switch on the frontend context global 0x850078.
u32 cardb_unlock_filter(int frontendCtx) {
	switch (frontendCtx) {
	case CARDB_CTX_ONLINE:
	case CARDB_CTX_ONLINE_CARSEL:
		return 0x1F8u;
	case CARDB_CTX_LAN:             // not in the switch: the default, 1
	case CARDB_CTX_QUICKRACE:
	case CARDB_CTX_SPLITSCREEN:
	case CARDB_CTX_CUSTOMIZE:
	default:
		return 1u;
	}
}

// The tail of SPEED2.EXE:0x4B2310, which writes the FILTERTYPE String (widget
// name hash 0x3E81DE59). The "All Cars" case is tested first and is an exact
// match on 3, not a bit test; the rest are bit tests in this order, so a filter
// with several bits set shows the last one that matched. All six keys resolve
// in the shipped English.bin.
u32 cardb_filter_label_hash(u32 filter) {
	if (filter == CARDB_FILTER_ALL)
		return 0x6B68362Cu; // "All Cars"
	u32 h = 0xE798A2F7u;    // "Stock Cars" -- the default arm
	if (filter & CARDB_FILTER_MY_CARS)
		h = 0xE7AB43D3u; // "My Cars"
	if (filter & CARDB_FILTER_CAREER)
		h = 0xB4045365u; // "Career"
	if (filter & CARDB_FILTER_SPONSOR)
		h = 0x26FECEE7u; // "SPONSOR CARS"
	if (filter & CARDB_FILTER_RANKED)
		h = 0x1A23D5E6u; // "Ranked Cars"
	return h;
}

int cardb_build_rows(const CarDb* db, int frontendCtx, int buildRegion,
		bool unlockAll, CarRow* rows, int maxRows) {
	if (!db || !rows || maxRows <= 0)
		return 0;

	// This is the composition of the two passes the engine runs, in its order.
	//
	// 1. SPEED2.EXE:0x516500 (DefaultStockCars) fills the screen's 48-slot
	//    record array by walking the whole car table and keeping every record
	//    that has a geometry path (+0x40), has bit 1 of +0xDA set, and is a
	//    player car (+0x844 == 0) -- or a traffic car (+0x844 == 2) when the
	//    debug global 0x838470 is set, which no shipped path sets. Each kept
	//    record gets filter mask 1 and its car type at +0x18, and the walk
	//    stops at 0x30 entries. On the shipped table that is car types 0..30:
	//    all 31 player cars.
	//
	// 2. SPEED2.EXE:0x4E22E0 (BuildCarList) then walks those records through
	//    0x5162D0 (accept when the record is present and its mask meets the
	//    screen's filter) and keeps one only when its car type resolves to a
	//    real CarTypeInfo with +0x840 != -1, 0x5022C0 says the region ships it,
	//    0x610180 says its artwork is on the disc, and 0x534180 says it is
	//    unlocked.
	//
	// The list is then handed to 0x43E650 with comparator 0x40CB20 -- which is
	// literally `MOV EAX,1 / RET`. 0x43E650 only reorders a pair when its
	// comparator returns 0, so the sort never swaps anything: the panel's order
	// is car-type order, and it is reproduced here by emitting in record order
	// rather than by porting a sort that cannot move an element.
	//
	// About `unlockAll`: it stands for one gate only, 0x534180, whose Quick
	// Race arm is `0x529F70(filter, handle) | 0x529CC0(handle)` -- "is this car
	// in the profile's owned list" OR "is it one of the eight starter cars at
	// 0x7F7C08 ({0, 1, 2, 5, 10, 25, 28, 22} for region 0)". 0x529F70 accepts
	// every ordinary stock car, so under the stock mask the whole list is
	// unlocked regardless of profile state. This port has no career-progress
	// list yet, so the caller passes true to take exactly that path rather than
	// invent one -- the same treatment, and the same reasoning, as
	// trackdb_build_rows().
	//
	// What it must NOT do is disable the region filter, even though the engine
	// happens to short-circuit both gates off one global (0x838464). That
	// global is the debug unlock-all, and 0x5022C0 is not an unlock at all: it
	// answers "does this disc ship this car", which no amount of career
	// progress changes. Passing unlockAll through to it put CORSA and PEUGOT106
	// on a US-build screen -- 31 rows where the retail game shows 29 -- so the
	// region test is called with false and the debug arm is left to the caller
	// that actually means it.
	//
	// The 0x610180 "artwork on disc" test is the one gate not reproduced here:
	// it stats the car's own CARS\<NAME>\ directory, and this port ships the
	// frontend art (FrontB.lzc) without the in-race car geometry, so running it
	// against the 3DS romfs would empty the list rather than reproduce the PC
	// screen. The record's own +0x40 geometry-path test above is the shipped
	// data's answer to the same question and every stock record passes it.
	// Both of these are resolved and then deliberately unused: 0x534180 is the
	// one gate whose inputs this port has but whose answer it cannot change --
	// under the stock mask that function accepts every row either way. They are
	// computed here rather than dropped from the signature so that the gate has
	// a place to go when a real owned-cars list exists.
	u32 filter = cardb_unlock_filter(frontendCtx);
	(void)filter;
	(void)unlockAll;

	int n = 0;
	int stock = 0;
	for (int i = 0; i < db->count && n < maxRows; i++) {
		const CarRec* r = &db->recs[i];

		// -- 0x516500 --
		if (!r->hasGeometry)
			continue;
		if (((r->flags >> 1) & 1) == 0)
			continue;
		if (r->kind != 0) // traffic needs the debug global 0x838470
			continue;
		if (stock >= CARDB_MAX_STOCK)
			break;
		stock++;

		// -- 0x4E22E0 --
		if (r->carType < 0)
			continue;
		if (!cardb_region_allows(r->carType, buildRegion, /*debugUnlockAll=*/false))
			continue;

		rows[n].carType = r->carType;
		rows[n].badgeHash = cardb_logo_hash(r, buildRegion, false);
		rows[n].logoHash = cardb_logo_hash(r, buildRegion, true);
		snprintf(rows[n].geomDir, sizeof(rows[n].geomDir), "%s", r->geomDir);
		memcpy(rows[n].rating, r->rating, sizeof(rows[n].rating));
		rows[n].ratingValid = r->ratingValid;
		rows[n].nameHash = r->nameHash;
		rows[n].stockPaint = r->stockPaint;
		rows[n].tunedSlot = -1;
		rows[n].careerSlot = -1;
		rows[n].sponsorIdx = -1;
		n++;
	}

	return n;
}
