#pragma once
#include <3ds.h>
#include <stdbool.h>

// Runtime parser + list builder for NFSU2's car database -- the table behind
// UI_QRCarSelect's one-car-at-a-time panel (TASKS.md #115 layer 3).
//
// Where the data lives
// --------------------
// The table is a top-level bChunk of type 0x00034600 inside GLOBAL/GLOBALB.BUN.
// The engine's loader is SPEED2.EXE:0x636BE0: it accepts that chunk type and
// takes the record array base as (chunk header + 0x17) & ~0xF -- the payload
// start rounded up to a 16-byte boundary -- storing it in the global
// [0x8A1CCC] that every reader below indexes. The extent is not read from the
// chunk header at all; it is the literal 0x189E0 the loader and
// DefaultStockCars (0x516500) both compare against, which at a 0x890 stride is
// exactly 46 records. In the shipped PC GLOBALB.BUN that chunk header sits at
// file offset 7101696, so the records start at 7101712 and run to 7101712 +
// 0x189E0, the end of the chunk.
//
// Indexing is [0x8A1CCC] + carType * 0x890 (0x890 == 2192), used verbatim at
// 0x516500, 0x4E22E0 and 0x511E60.
//
// Nothing here is invented: every field offset, filter bit, texture-name format
// and language key below was recovered from SPEED2.EXE and cross-checked
// against the shipped PC GLOBALB.BUN / FRONTEND/FrontB.lzc / LANGUAGES/
// English.bin (see cardb.c).
//
// Record layout (0x890 bytes), fields this port needs:
//   +0x000 char[32] name          "350Z" -- the SECONDARY_LOGO_ key (0x511E60)
//   +0x040 char[32] geometry path "CARS\350Z\GEOMETRY.BIN"; empty means the car
//                                 has no art on this disc, and 0x516500 skips
//                                 the record outright
//   +0x0C0 char[32] manufacturer  "NISSAN" -- the CARSELECT_MANUFACTURER_ key
//   +0x0D0 dword    name hash     FEHashUpper(+0x00), matches every record
//   +0x0DA byte     flags         bit 1 = selectable player car (0x516500)
//   +0x840 dword    carType       the record's own index, 0..45
//   +0x844 dword    kind          0 = player car, 2 = traffic
//
// Drivetrain block (TASKS.md #142). Recovered this session by scanning the
// undecoded remainder of the record across all 46 entries for fields whose
// value ranges and per-car variation could only be one thing, then checking
// each candidate against the others for internal consistency:
//
//   +0x2C8 float    final drive   3.27..4.80, duplicated at +0x2CC
//   +0x2D8 dword    forward gears 4, 5 or 6
//   +0x2E0 float[8] gear ratios   [reverse, neutral, 1st..6th]; reverse is
//                                 stored negative and neutral is a literal 0,
//                                 so the array is indexed by a signed gear
//                                 number with no special cases
//   +0x300 float    idle rpm      800 or 850
//   +0x304 float    redline rpm   6500..8000
//   +0x308 float    limiter rpm   always ~500 over redline
//   +0x310 float[9] torque curve  kN m, sampled evenly idle..limiter, and
//                                 peaked in the middle like a real curve
//
// This is read data, not a port invention, and it is self-validating: the gear
// COUNT at +0x2D8 independently equals the number of non-zero forward ratios at
// +0x2E8 for every one of the 46 records, which a misread offset would not
// survive. The final drives are also real manufacturer figures (240SX 4.083,
// MIATA 4.30, CIVIC 4.40), and the ratio ladders descend monotonically.
//
// The torque curve is NOT normalised: it is absolute, in kN m, the same
// tonne-scale unit family the mass (tonnes) and inertia (tonne m^2) below use.
// Read that way the curves reproduce the real cars' published figures -- 350Z
// 370 Nm / 287 hp against a real 371 / 287, IS300 294 / 214 against 298 / 215,
// RSX 192 / 200 against 196 / 200, HUMMER 522 Nm against 515 -- with a mean
// error of 9% whose outliers are exactly the turbo cars (SUPRA, SKYLINE, RX7,
// LANCEREVO8, IMPREZAWRX) that NFSU2 deliberately ships detuned in stock form.
//
// An earlier pass concluded mass was not in the record. That was wrong, and the
// reason is the same units: a scan for a kilogram-range field found nothing
// because the mass is stored in tonnes. See CarChassis below.

// 46 in the shipped PC data; headroom for other SKUs' tables.
#define CARDB_MAX_RECORDS 64

// SPEED2.EXE:0x516500 -- the stock list is capped at 0x30 entries.
#define CARDB_MAX_STOCK 0x30
// The car panel's row arrays: the stock list plus Customize's tuned cars
// (FEPlayerCarDB's 20 TUNED_CAR_%d slots, #379), which the panel lists after
// the stock cars under the All Cars / My Cars filters.
#define CARDB_MAX_ROWS (CARDB_MAX_STOCK + 0x18)

// The real per-car drivetrain, straight out of the record (see the offsets
// above). Everything a gearbox and a tachometer need; nothing is derived here,
// so a field that reads as zero means the shipped table has a zero in it.
#define CARPERF_MAX_GEARS 6
#define CARPERF_TORQUE_POINTS 9

typedef struct {
	float finalDrive;                       // +0x2C8
	int   numGears;                         // +0x2D8, forward gears only
	float reverseRatio;                     // +0x2E0, stored negative
	float gearRatio[CARPERF_MAX_GEARS];     // +0x2E8, 1st..6th
	float idleRpm;                          // +0x300
	float redlineRpm;                       // +0x304
	float limiterRpm;                       // +0x308
	float torque[CARPERF_TORQUE_POINTS];    // +0x310, kN m (x1000 for N m)
	bool  valid;                            // the record carried a usable one
} CarPerf;

// Engine torque at `rpm` in kN m, by linear interpolation between the nine
// sampled points, which are spread evenly from idle to the limiter. Returns 0
// outside the curve and for a record with no drivetrain, so a caller that
// forgets to check `valid` gets no thrust rather than a divide by zero.
float carperf_torque_at(const CarPerf* p, float rpm);

// Road speed (m/s) the engine turns at `rpm` in `gear` (1..numGears, 0 =
// neutral, -1 = reverse), and the inverse. `wheelRadius` is in metres. These
// are the only two places the ratios are used, so the sign convention for
// reverse lives here and nowhere else.
float carperf_speed_at_rpm(const CarPerf* p, int gear, float rpm, float wheelRadius);
float carperf_rpm_at_speed(const CarPerf* p, int gear, float speed, float wheelRadius);

// The rigid body, also straight out of the record. Found by the same scan that
// turned up the drivetrain, and like it, self-validating:
//
//   +0x120 CarWheel[4]  0x30 stride, in the order FL, FR, RR, RL. Each entry is
//                       a position (+0x00 x forward, +0x04 y LEFT, +0x08 z up,
//                       all from the centre of mass), a rolling radius at +0x10,
//                       a tyre width at +0x14, and the wheel's own index 0..3 at
//                       +0x18 -- that stored index is what proves the stride.
//   +0x220 float        mass, in TONNES
//   +0x224 float[3]     body length, width, height, in metres
//   +0x230 float        Ixx (roll)    the inertia tensor diagonal, in tonne m^2,
//   +0x244 float        Iyy (pitch)   on a 0x14 stride with zero off-diagonals
//   +0x258 float        Izz (yaw)
//   +0x30C float        read as aerodynamic drag; really the engine inertia
//                       (see CarChassis::dragCoef and CarPhysInfo)
//
// The +0x30C field (0.0002, 0.00035, 0.0006, 0.0007, in round numbers) was once
// taken for aerodynamic drag because it ranks the fleet roughly by body size.
// It is not: retail's EngineInfo is CarTypeInfo + 0x300, and its +0xC is the
// engine's moment of inertia, which UpdateCar divides SmoothRPMDecel by
// (TASKS.md #222). The real aero block is in CarPhysInfo.
//
// Why this is certainly right, and not three plausible-looking floats:
//
//   * The stored inertia equals the uniform-box tensor of the stored mass and
//     the stored dimensions -- m(W^2+H^2)/12, m(L^2+H^2)/12, m(L^2+W^2)/12 --
//     to a worst relative error of 7.6e-08 across all 46 records and all three
//     axes, with no exceptions. Only one set of offsets can do that.
//   * The masses are real curb weights once read as tonnes, which is why an
//     earlier scan of this record for a kilogram-range field found nothing:
//     240SX 1.220, MIATA 1.040, CIVIC 1.180, RX7 1.220, HUMMER 2.900,
//     NAVIGATOR 2.700, ESCALADE 2.500, LANCEREVO8 1.400.
//   * The wheel positions give real wheelbases and tracks (MIATA 2.29 m /
//     1.40 m, 350Z 2.66 m / 1.51 m, HUMMER 3.11 m / 1.76 m) and the tyre widths
//     are the real fitments, including the staggered rears the 350Z and the
//     HUMMER actually have (0.225 front / 0.235 rear, 0.285 / 0.315).
//   * The body dimensions are the real cars to the centimetre (HUMMER
//     4.819 x 2.062 x 1.977).
//
// So carsim.c does not have to invent a chassis: mass, inertia, wheelbase,
// track, weight distribution and rolling radius are all shipped data. The tyre
// friction, suspension, aero and drivetrain numbers carsim.c runs on are not in
// this struct: they come from the CarPhysInfo block below.
#define CARPHYS_WHEELS 4

typedef struct {
	float pos[3];     // +0x00, metres from the CG: x forward, y LEFT, z up
	float radius;     // +0x10, rolling radius
	float tyreWidth;  // +0x14
} CarWheel;

typedef struct {
	float mass;             // kg -- converted from the record's tonnes
	float length, width, height;   // m, the body box
	float inertia[3];       // kg m^2 -- roll, pitch, yaw
	CarWheel wheel[CARPHYS_WHEELS];

	// Derived from the wheel array, so that callers never repeat the geometry.
	float wheelbase;
	float trackFront, trackRear;
	float cgToFront, cgToRear;   // both positive
	float wheelRadius;           // mean of the four, for the gearing maths

	// +0x30C. Read as "aerodynamic drag" by an earlier pass, which was wrong:
	// retail's EngineInfo is CTI+0x300 and its +0xC is the engine's moment of
	// inertia (UpdateCar divides SmoothRPMDecel by it; TASKS.md #222). Kept only
	// for the load log; carsim.c takes the real aero block out of CarPhysInfo.
	float dragCoef;

	bool valid;
} CarChassis;

// Retail's whole physics block (TASKS.md #222). This record is the GameCube
// Speed.elf's CarTypeInfo, and its CarPhysicsInfo is CarTypeInfo + 0x110 for
// 0x2B0 bytes (RebuildPhysicsInfo 0x801C89A8): CG offset, the four WheelInfo,
// both TireInfo, the RigidBodyInfo, both SuspensionInfo, DriveTrainInfo,
// EngineInfo, AeroInfo, the control block and the speed governor.
// docs/retail_car_physics.md has every offset; carsim.c reads them by those
// offsets, relative to the start of this block, so its CPI_* names match the
// decompile one for one. Little-endian as shipped (the ARM11 is too). For a
// player car (kind 0) cardb_load() keeps the stock build of it, not the
// shipped bytes: RebuildPhysicsInfo with nothing fitted, which is what retail
// drives (TASKS.md #288; ridephys.h). Traffic records keep the shipped block.
#define CARPHYS_CPI_OFFSET 0x110
#define CARPHYS_CPI_SIZE   0x2B0

typedef struct {
	u8 raw[CARPHYS_CPI_SIZE];
	// CarTypeInfo +0xD8 (s8): retail's RigidBody+0x148 / Engine+0x74, which
	// adds 5% torque and 3% to the governor. Zero on every shipped car.
	s8 boostFlag;
	bool valid;
	// The car's default collision hull (carhull.h): hullN vertices, three
	// floats each, model space; NULL and 0 without GLOBAL/CARHULLS.nfsg.
	// Retail's CarCollisionBody is this polytope, not the body box above.
	const float* hull;
	int hullN;
} CarPhysInfo;

// One camera point of view out of the record's CameraInfo, CarTypeInfo +0x720
// (Speed.elf Car::GetPov 0x80224160 / CameraInfo::GetPov 0x8005FCE8): a count
// at +0, then 0x10-byte POVs from +0x10 laid out {s16 type, u16 angle, float x,
// float z, float y}. The eye is in the car's own frame (x front, y left, z up,
// metres from the record origin) and `angle` tips the look down, in
// 1/65536ths of a turn. Every shipped car carries all six types.
#define CARDB_POV_MAX 6
#define CARDB_POV_FAR_CHASE 0   // the default: PlayerSettings camera 3
#define CARDB_POV_NEAR_CHASE 1
#define CARDB_POV_BUMPER 2
#define CARDB_POV_IN_CAR 3
#define CARDB_POV_HOOD 4
#define CARDB_POV_HIGH_CHASE 5

typedef struct {
	s16 type;
	u16 angle;
	float eye[3];   // x, y, z -- reordered from the record's x, z, y
} CarPov;

typedef struct {
	char name[32];         // +0x000
	char manufacturer[32]; // +0x0C0
	u32 nameHash;          // +0x0D0
	u8 hasGeometry;        // +0x040 non-empty
	// The directory component of the +0x040 geometry path, i.e. the "350Z" of
	// "CARS\350Z\GEOMETRY.BIN". That is the folder tools/car_pack.py is pointed
	// at and the basename it writes, so this is the key that gets a record's
	// real 3D model off the SD card. Empty when hasGeometry is 0. Taken from
	// the path rather than assumed equal to `name` -- the two happen to match
	// on all 31 player records, but the path is the field the engine actually
	// opens.
	char geomDir[32];
	u8 flags;              // +0x0DA
	// +0x0DC..+0x0DE, signed: the rims the car takes. GetRandomWheel
	// (Speed.elf 0x80246D94) keeps a wheel part whose OUTER_RADIUS is
	// wheelOuter and whose INNER_RADIUS lies in wheelInnerMin..Max.
	s8 wheelOuter;
	s8 wheelInnerMin;
	s8 wheelInnerMax;
	int carType;           // +0x840
	int kind;              // +0x844
	// +0x84C, the stock body paint part: SetStockParts (Speed.elf 0x80244784)
	// puts it in the BASE_PAINT slot when a stock car is built with set 0,
	// which is every FillFromStockCar -- i.e. the paint a car comes out of
	// the car lot in (PEUGOT's GLOSS_L2_COLOR03, 0x2D392EE5). +0x850 is the
	// set-1 alternative nothing on the career path asks for.
	u32 stockPaint;
	// A traffic car's (kind 2) streaming fields (TASKS.md #316). +0x854, one
	// per zone traffic type (TrackPathZone +0x3C): the weight
	// StreamingTrafficCarManager::Update picks the type by; +0x859, set where
	// the type must not be loaded in that zone type. +0xD9 is how many skins
	// the car has and +0x880 their numbers, 1-based (the <CAR>_SKINn
	// textures), which CarTypeInfo::GetRandomSkin (0x802441D4) draws from.
	#define CARDB_TRAFFIC_ZONE_TYPES 5
	#define CARDB_MAX_SKINS 8
	u8 trafficWeight[CARDB_TRAFFIC_ZONE_TYPES];
	u8 trafficNoLoad[CARDB_TRAFFIC_ZONE_TYPES];
	s8 skinCount;
	s8 skins[CARDB_MAX_SKINS];
	CarPerf perf;          // the +0x2C8..+0x330 drivetrain block
	CarChassis chassis;    // the +0x120..+0x258 rigid body
	CarPhysInfo phys;      // the +0x110..+0x3C0 retail physics block
	CarPov pov[CARDB_POV_MAX];   // the +0x720 CameraInfo
	int povCount;
	// CarHudInfo (+0x7F0) +4..+0x10, the tach needle's sweep in degrees:
	// [0]/[1] the start and end CalcAngleForRPMDrag takes with more than one
	// player, [2]/[3] the one-player drag tach's (Speed.elf 0x8010D000). The
	// second pair is -43/43 on every PC record.
	float hudAngle[4];

	// The car's own performance-config table, out of the 0x80034A30 container
	// described under CarDb below: `measureStock` is its row 0 and
	// `measureMax` its last row, i.e. the raw measure stock and fully
	// upgraded. Zero on the fifteen traffic records, whose table is one empty
	// row. Raw -- cardb_perf_measure() is what puts it on retail's 0..10 axis.
	float measureStock;
	float measureMax;
	bool  measureValid;
	// Where the whole table sits in GLOBALB.BUN and how many rows it has, for
	// cardb_read_perf_rows(): the AI's MatchPerformance searches every row.
	long  perfRowsAt;
	int   perfRowCount;

	// The stock car's three bar ratings, RIDEPHYS_RATING_* order (accel, top
	// speed, handling) on retail's 0.03..1 scale: ridephys.c's port of
	// RidePhysicsInfo, built with nothing installed (TASKS.md #116).
	float rating[3];
	bool  ratingValid;
} CarRec;

#define CARDB_EXTREMA_FLOATS 30

typedef struct {
	CarRec recs[CARDB_MAX_RECORDS];
	int count;

	// The fleet's measure range, Extrema+0x28 and +0x2C (Speed.elf
	// 0x803DDA64/68), which every normalized measure in the game is scaled by.
	// In the shipped data they are 144.1214 and 613.8965 -- the COROLLA stock
	// and the fully-upgraded FOCUS, the slowest and fastest the table holds.
	float measureLo, measureHi;
	bool  measureOk;

	// All of Extrema (0x803DDA3C), the 0x34A32 chunk's 30 floats: the fleet
	// ranges every rating is normalised by (ridephys.h).
	float extrema[CARDB_EXTREMA_FLOATS];
	bool  extremaOk;

	// The file offset of record 0 in GLOBALB.BUN, for cardb_read_compleat().
	long recBase;
} CarDb;

// Frontend context values the engine keeps in global 0x850078 -- the same
// global trackdb.h documents. UI_QRCarSelect is shared between Quick Race and
// Customize and tells them apart by exactly this, so both are named here even
// though only Quick Race is reachable in this port today.
#define CARDB_CTX_QUICKRACE 0x2
#define CARDB_CTX_SPLITSCREEN 0x4
#define CARDB_CTX_ONLINE 0x8          // 0x200 is LAN, see TRACKDB_CTX_LAN
#define CARDB_CTX_CUSTOMIZE 0x20
#define CARDB_CTX_ONLINE_CARSEL 0x100 // online's car select (lan-lobby-pc-re.md)
#define CARDB_CTX_LAN 0x200

// Car-filter bits, from the per-player filter global 0x83AAC0 and the
// filter -> FILTERTYPE-string table at the tail of 0x4B2310.
#define CARDB_FILTER_STOCK 0x1
#define CARDB_FILTER_MY_CARS 0x2
#define CARDB_FILTER_CAREER 0x4
#define CARDB_FILTER_SPONSOR 0x8
#define CARDB_FILTER_RANKED 0x10
#define CARDB_FILTER_ALL 0x3

// One built row of the car panel, in the engine's own order.
typedef struct {
	int carType;   // index into the record array; the value a pick commits
	u32 badgeHash; // CarBadge texture -- SPEED2.EXE:0x511E60(carType, 0)
	u32 logoHash;  // ManufacturerLogo texture -- 0x511E60(carType, 1)
	// Copied off the record so the caller can load this row's 3D model without
	// keeping the whole CarDb resident -- see CarRec::geomDir.
	char geomDir[32];
	// The record's stock ratings (CarRec::rating), for the stats plate's bars.
	float rating[3];
	bool  ratingValid;
	// CarRec::nameHash and ::stockPaint, so a stock row's car can be built the
	// way FEStockCar::BuildRide builds it (#296) without the CarDb either.
	u32 nameHash;
	u32 stockPaint;
	// The profile's tuned-car slot this row shows (Customize's My Cars), or -1
	// for a stock row. cardb_build_rows() leaves it -1.
	signed char tunedSlot;
	// The profile's career slot this row shows (Quick Race's Career view,
	// #390), or -1. cardb_build_rows() leaves it -1.
	signed char careerSlot;
	// The Sponsor car this row shows (Quick Race's Sponsor view, #445: an index
	// into DefaultSponsorCars' list), or -1. cardb_build_rows() leaves it -1.
	signed char sponsorIdx;
} CarRow;

// Scans `bunPath` (GLOBAL/GLOBALB.BUN) for the 0x34600 chunk and decodes it,
// then for the 0x80034A30 performance-config container (see below). Reads only
// the 8-byte chunk headers plus the fields below out of the matching payloads,
// so this never mallocs the whole ~8MB container.
bool cardb_load(const char* bunPath, CarDb* out);

// The CompleatCarPhysicsInfo of the record whose carType is `carType` --
// RIDEPHYS_COMPLEAT_SIZE bytes from CarTypeInfo +0x110, the stock physics and
// every upgrade target -- read off the card into `out`, for building a car
// with parts fitted. Kept out of CarRec because only the one car being shown
// ever needs it, and 46 of them would add 40 KB to every CarDb.
bool cardb_read_compleat(const char* bunPath, const CarDb* db, int carType, u8* out);

// The car's whole performance-config table (the 0x34A31 sub-chunk described
// below), read off the card into `rows`: CARDB_PERF_ROW_SIZE bytes a row, at
// most `maxRows` of them. Each row is a PhysicsUpgradePackedSpec -- the
// measure at +0x00, and at +0x05 the installed-part bits that
// UnpackUpgradeInfo (Speed.elf 0x801C417C) reads, bit 7 - (id & 7) of byte
// id >> 3 (IsPerfPartInstalled, 0x801C4098). Row 0 is stock and the rows
// climb to fully upgraded. Returns the number read, 0 for a car with no table.
#define CARDB_PERF_ROW_SIZE 0x10
#define CARDB_PERF_MAX_ROWS 64
int cardb_read_perf_rows(const char* bunPath, const CarDb* db, int carType, u8* rows,
	int maxRows);

// RidePhysicsInfo::GetPerformanceMeasure(-1.0f, car, normalize = true)
// (Speed.elf 0x801C7EF8) for a STOCK car: the measure on the 0..10 axis every
// comparison in the game is made on,
//
//     (raw - Extrema[0x28]) / (Extrema[0x2C] - Extrema[0x28]) * 10
//
// with the 10 being the constant at 0x80332BE0. The -1.0f every shipped caller
// passes is below the 0.0 gate at 0x80332BD8, so the handling-rating term and
// the GetStockPerformanceMeasure floor under it never run and the raw figure is
// the car's own table row 0 -- which is exactly what GetStockPerformanceMeasure
// returns anyway.
//
// Where the numbers come from
// ---------------------------
// LoaderPerformanceConfigChunk (0x801C3688) reads a shipped container, not a
// runtime computation:
//
//   0x80034A30  the container, in GLOBALB.BUN (PC file offset 7770112)
//   0x00034A31  one per car, in FILE ORDER -- the loader indexes them by its
//               own running counter, capped at 0x2E, so the n-th sub-chunk is
//               car type n. Payload: u32 row count, then that many 0x10-byte
//               rows whose first float is the measure at that upgrade level.
//               Row 0 is stock, the last row fully upgraded (which is what
//               GetMinMaxPerformanceMeasure 0x801C9748 returns as the pair).
//   0x00034A32  120 bytes copied straight into Extrema -- 30 floats of fleet
//               ranges, of which [10] and [11] are the measure's.
//   0x00034A33  0x10C-byte records; not read here.
//
// So RebuildPhysicsInfo's own integral of (acceleration - aero drag) over
// speed, which is how the raw measure was produced in the first place, does
// not have to be re-run: its answer ships with the game.
//
// Returns -1 for a car with no table (the traffic records) and for a car table
// that carries no 0x80034A30 container at all, which callers read as "no
// measure" rather than as a very slow car.
float cardb_perf_measure(const CarDb* db, int carType);
// DAT_80332BE0, the constant GetPerformanceMeasure's normalize path scales by:
// the top of that 0..10 axis.
#define CARDB_MEASURE_SCALE 10.0f

// SPEED2.EXE:0x570A30 -- the build region, global [0x864F24]. Every region
// override in this file is keyed off it. 0 = US, retail PC's; 1 and 5..9 =
// Europe; 2 = Korea; 3 = Japan; 4 = Taiwan; 10 = Korea (English); 11 = China;
// 12 = Thailand (SPEED2.EXE 0x5712A0's country codes).
#define CARDB_REGION_MAX 12
// PORT (#455): Region=all, no retail region. Every region's cars ship; where
// the regions disagree about a car (its badge, its logo, its career unlock,
// whether it is a starter) the US arm wins, and the two cars the US build does
// not ship (the 106 and the Corsa) take the European one. The AI picks from
// both retail lists, and the Best Buy vinyl stays available.
#define CARDB_REGION_ALL 13
int cardb_build_region(void);
// PORT (#415): the config's Region key. Out of range means 0.
void cardb_set_build_region(int region);
// PORT (#455): the retail region whose arm a per-car table takes for this car:
// `region` itself, or under CARDB_REGION_ALL 0 (US), or 1 (Europe) for a car
// region 0 does not ship.
int cardb_region_for_car(int region, int carType);

// SPEED2.EXE:0x5022C0 -- is this car type shipped in the current build region?
// SPEED2.EXE:0x5022C0 -- "does this build region ship this car". The third
// argument is the engine's own debug/cheat global 0x838464, which short-circuits
// this test to "yes"; no shipped code path sets it, so callers pass false. It is
// NOT the same thing as the unlockAll cardb_build_rows() takes: that one stands
// in for the career-progress list behind 0x534180, and routing it into this test
// would hand the player cars the disc does not ship.
bool cardb_region_allows(int carType, int buildRegion, bool debugUnlockAll);

// SPEED2.EXE:0x511E60 -- the two texture hashes for one car. `manufacturer`
// selects the CARSELECT_MANUFACTURER_ logo over the SECONDARY_LOGO_ badge.
u32 cardb_logo_hash(const CarRec* r, int buildRegion, bool manufacturer);

// FEHashUpper("GENERICPLACEHOLDER") -- what 0x4B2310 substitutes when the
// computed hash names no texture in the pack.
u32 cardb_placeholder_hash(void);

// GetCarModelNameHash (Speed.elf 0x8011D80C): FEngHashString("CAR_NAME_%s")
// over the record's +0x000 name, i.e. the English.bin key of the car's display
// name ("CAR_NAME_TIBURON" -> "Hyundai Tiburon GT"). SPEED2.EXE builds the
// same key inline at each caller off the format at .rdata 0x790400.
u32 cardb_name_label_hash(const CarRec* r);

// SPEED2.EXE:0x497DA0 -- the unlock filter for a frontend context.
u32 cardb_unlock_filter(int frontendCtx);

// The tail of SPEED2.EXE:0x4B2310 -- English.bin key for the FILTERTYPE field.
u32 cardb_filter_label_hash(u32 filter);

// Builds the panel's car list the way SPEED2.EXE:0x516500 + 0x4E22E0 do:
// take the stock list (every record with art whose +0xDA bit 1 is set), keep
// what the region and the unlock filter accept, in car-type order. `unlockAll`
// stands in for the engine's global 0x838464 -- see cardb.c. Returns the number
// of rows written, capped at `maxRows`.
int cardb_build_rows(const CarDb* db, int frontendCtx, int buildRegion,
	bool unlockAll, CarRow* rows, int maxRows);
