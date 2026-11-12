#pragma once
#include <3ds.h>
#include "eventdb.h"

// Runtime parser + row builder for NFSU2's track database -- the authored table
// behind UI_QRTrackSelect's selection ring (TASKS.md #115 layer 1).
//
// Where the data lives
// --------------------
// The table is a top-level bChunk of type 0x00034201 inside GLOBAL/GLOBALB.BUN.
// The engine's own loader is SPEED2.EXE:0x5D3EB0: it accepts that chunk type,
// takes the record array base as (chunk header + 8) -- i.e. the chunk's payload
// -- and the record count as (chunk size / 296). In the shipped PC GLOBALB.BUN
// that chunk sits at file offset 0x735F00 with size 38776 == 131 * 296, so the
// records fill the payload exactly. Indexing is SPEED2.EXE:0x5D3E80,
// `[0x883DA0] + i * 0x128` (0x128 == 296), and lookup-by-trackId is 0x5D3E40.
//
// Nothing here is invented: every field offset, filter bit, icon hash and
// language key below was recovered from SPEED2.EXE and cross-checked against
// the shipped PC GLOBALB.BUN / LANGUAGES/English.bin (see trackdb.c).
//
// Record layout (296 bytes), fields this port needs:
//   +0x00 char[32] dev name       "(4011) Short Circuit #1"
//   +0x20 char[32] track path     "Location4\Track_4011"
//   +0x40 char[]   region         "L4RA": the track set the track loads with.
//                                  SPEED2.EXE formats it into TRACKS\%s.BUN
//                                  (0x57DD4B) and TRACKS\STREAM%s.BUN
//                                  (0x57A5A8), off the loading track's record
//                                  ([0x883DAC] + 0x40), and BeginGameFlow-
//                                  SwitchTrack (Speed.elf 0x801971E8) reloads
//                                  the region only when the old and new
//                                  records' names differ (bStrCmp). Shipped:
//                                  L4RA the city, L4RB/L4RD drags, L4RC drift,
//                                  L4RF Street X, L4RG the URL speedways,
//                                  L4RH the dyno, L4RR the replay cameras.
//   +0x7C dword    region icon    0..5, selects the location icon (0x4984B0)
//   +0x80 dword    traffic flag    1 -> the mode-options screen offers a Traffic
//                                  row on this track (drift builder 0x4CD7EE)
//   +0x85 byte     point to point  1 on every sprint and drag: the race has a
//                                  finish line instead of a lap count (Race
//                                  params +0x18, FigureOutBoundaries)
//   +0x86 byte     reverse flag    1 -> the track can be driven reversed, which
//                                  is what gates the Track Direction option row
//                                  (every builder, e.g. 0x4CCFCD)
//   +0x88 byte     exclude        non-zero -> never listed (0x4987C0)
//   +0x8A word     track id       4000..4901; the language-key and TRACKMAP id
//   +0x94 dword    race type mask bit per race type (0x4987C0 / table 0x498880)
//   +0x98 dword    distance       metres (0x4CE355)
//   +0x9C float    par time       milliseconds, kind 0 (0x502AA0's fallback)
//   +0xA0 float    par time       milliseconds, kind != 0
//   +0xA4 dword    par score      kind 0 (0x502C50's fallback)
//   +0xA8 dword    par score      kind != 0
//   +0xAC float    map origin X   world X at the TRACKMAP art's left edge
//   +0xB0 float    map origin Y   world Y at the art's top edge
//   +0xB4 float    map span       world units the (square) art covers
//   +0xC0 float    map zoom       the minimap window is 0.3 / zoom of the art
//
// The last four are the minimap's, recovered from the GameCube build rather
// than SPEED2.EXE because Ghidra cannot decompile the functions that read them
// (Gekko paired-single opcodes -- see tools/gc_disasm.py):
//   Minimap::Tick                               Speed.elf 0x80115fbc
//   Minimap::GetScreenPositionForWorldCoordinate          0x8011668c
// Both treat the art as a square window on the world, so a world point maps to
// art UV as ((wx - originX) / span, (originY - wy) / span) -- note the Y flip:
// +0xB0 is the TOP edge, and the art's V axis runs south. Checked against the
// shipped PC GLOBALB.BUN: all 67 Location4 (Bayview) records carry the same
// (-3425.43, 3287.26, 5633.16, zoom 1.5), and the user's own career respawn
// (-992.01, 817.98) lands at UV (0.432, 0.438) -- the middle of the city, which
// is where that save's car is parked.

// 131 in the shipped PC data; headroom for other SKUs' tables.
#define TRACKDB_MAX_RECORDS 192

typedef struct {
	u16 trackId;      // +0x8A
	char regionName[8]; // +0x40, "L4RA"
	u8 regionIcon;    // +0x7C
	u8 exclude;       // +0x88
	u8 allowsTraffic; // +0x80 != 0
	u8 allowsReverse; // +0x86
	u8 pointToPoint;  // +0x85
	u32 raceTypeMask; // +0x94
	u32 distanceM;    // +0x98
	float parTimeMs[2];  // +0x9C, +0xA0
	s32 parScore[2];     // +0xA4, +0xA8
	float mapOriginX;    // +0xAC
	float mapOriginY;    // +0xB0
	float mapSpan;       // +0xB4
	float mapZoom;       // +0xC0
	// The world map's SelectableTrack view (PlayableEvent ctor, SPEED2.EXE
	// 0x4B1200): the TRACKMAP art's zoom window, in 512-unit art space, that
	// TrackMapScroller::ZoomIn (0x498630) closes in on, and whether it snaps
	// there instead of animating. Zero on most records (4000, 4201, 4301);
	// 4011 is (131, 202) at 256.
	float selCentreX;    // +0x118
	float selCentreY;    // +0x11C
	float selZoom;       // +0x120
	u8 selSnap;          // +0x124 != 0
	// +0xEC, how many traffic cars the track runs, by [reversed + level * 2]
	// for the four traffic levels: RaceStarter::AddTrafficCars (Speed.elf
	// 0x80119E20) adds min(16, this) of them. Track 4000, free roam, carries
	// 12 12 10 10 12 12 16 16, and StartCareerFreeRoam sets level 3: 16 cars.
	#define TRACKDB_TRAFFIC_LEVELS 4
	u8 trafficCars[TRACKDB_TRAFFIC_LEVELS * 2];
	// +0xF4 + reversed: when 0, CanPlaceCarHere (Speed.elf 0x8002F130) puts
	// no traffic car within +0xF8[reversed] metres past the start line before
	// the race starts. 0 on every record but 4174-4179's forward byte.
	u8 trafficStartOk[2];
	// +0xF6, signed: the track's car configuration, which a race-start NIS
	// candidate's +8 must match (CRaceStartAnimSelectionContext::Build, SpeedPS
	// 0x80045968; DoesCarConfigPass 0x80045A58, #244). 1 on most race tracks,
	// 2 / 3 / 6 / 7 (the drags) on the rest.
	s8 carConfig;
	// +0xF8 + reversed * 4: that distance, m. 500, or 100 on 4201/4401/4402/
	// 4404.
	float trafficStartClear[2];
	// +0x100 + reversed * 4, the spacing PlaceNewCarOntoRaceTrack (Speed.elf
	// 0x8002D0B0) keeps between the cars it first puts down, m: 31 on every
	// record, both ways.
	float trafficSpacing[2];
	// +0x108 + level * 4: the share of traffic cars WantTrafficCarGoingForwards-
	// WRTRace (0x8002F4C8) sends against the race. 0.5 everywhere but 4401
	// (0.5 0.6 0.7 0.8).
	float trafficOncoming[TRACKDB_TRAFFIC_LEVELS];
	// +0xE4, a short: when a racer may take a fork (AIGoal::
	// DecideSignpostAsRacer 0x8000A914, TASKS.md #409). -1, once Race::
	// GracePeriodIsActive is over; otherwise once the race clock is past this
	// many seconds. -1 on every race track but 4122's 0.
	s16 forkAfterSec;
} TrackRec;

typedef struct {
	TrackRec recs[TRACKDB_MAX_RECORDS];
	int count;
} TrackDb;

// Frontend context values the engine keeps in global 0x850078. Quick Race is
// the only one this port reaches today; the others are listed because several
// of the recovered functions branch on them (LAN/Online read the race type from
// 0x850088). LAN is 0x200 and online 8: UI_OLMAIN's builder 0x4BC7B0 titles
// itself "LAN Main" (0x03F6F2BF) only at 0x200 (docs/formats/lan-lobby-pc-re.md).
#define TRACKDB_CTX_QUICKRACE 0x2
#define TRACKDB_CTX_SPLITSCREEN 0x4
#define TRACKDB_CTX_ONLINE 0x8
#define TRACKDB_CTX_LAN 0x200

// One built row of the selection ring, in the engine's own order.
typedef struct {
	u16 trackId;
	bool locked;   // node+8: !IsUnlocked(ctx, trackId) -- SPEED2.EXE:0x4B4C10
	u32 iconHash;  // texture hash for the ring slot -- SPEED2.EXE:0x4984B0
	u32 distanceM; // record +0x98, metres
	s32 bestTime;  // 1/4000 s ticks -- see trackdb_best_time_ticks()
	s32 bestScore; // points -- see trackdb_best_score()
} TrackRow;

// The info panel's "Best Lap:" value. SPEED2.EXE:0x502AA0 scans the profile's
// best-time table -- 320 8-byte records at 0x844ADC, `{u16 trackId; u8 pad;
// u8 kind; s32 value}` -- for the *smallest* value matching (trackId, kind),
// and 0x502C50 scans the same table for the *largest* when the race type wants
// a score instead of a time. This port has no profile save, so that table is
// empty, every scan misses, and both drop to the engine's own miss path: the
// par values baked into the track record itself. That fallback is what these
// two reproduce.
//
// 0x502AA0's fallback is `(int)(record_float * 0.001f * 4000.0f + 0.5f)` --
// constants at 0x78435C and 0x784264/0x784260 -- i.e. the field is a par time
// in milliseconds and the result is in the engine's 1/4000 s tick unit.
// 0x502C50's is the raw int at +0xA4 / +0xA8.
//
// `kind` is the selection node's +0x10. SPEED2.EXE:0x4B4C10 zero-initialises
// it when it allocates the node and nothing on the track-select path ever
// writes it, so the caller at 0x4CE3E9 / 0x4CE44B always passes 0; the kind-1
// fields are parsed anyway because they are real record fields.
//
// In the shipped PC GLOBALB.BUN all four par fields are zero on all 131
// records, so both of these return 0 there -- which is exactly why retail's
// panel reads "0.00" on every track.
s32 trackdb_best_time_ticks(const TrackRec* r, int kind);
s32 trackdb_best_score(const TrackRec* r, int kind);

// SPEED2.EXE:0x5D3E40 -- the record for one track id, NULL when the table has
// no such row. The engine's Minimap holds the result of exactly this call at
// its +0x14 and re-fetches it whenever the current track changes.
const TrackRec* trackdb_find(const TrackDb* db, u16 trackId);

// Scans `bunPath` (GLOBAL/GLOBALB.BUN) for the 0x34201 chunk and decodes it.
// Reads only the 8-byte chunk headers plus the one matching payload, so this
// never mallocs the whole ~8MB container.
bool trackdb_load(const char* bunPath, TrackDb* out);

// SPEED2.EXE:0x4987C0 -- does `r` belong in the list for `raceType`? `secondary`
// is the engine's second argument, 0 on the track-select populate path
// (0x4CDED0), which is what gates the drag/drift/0xC cases.
bool trackdb_race_type_allows(const TrackRec* r, int raceType, int secondary);

// SPEED2.EXE:0x4984B0 -- the ring slot's texture hash for one row.
u32 trackdb_row_icon_hash(const TrackRec* r, int raceType, bool locked);

// SPEED2.EXE:0x498970 -- English.bin key for the RACE_TYPE info field.
u32 trackdb_race_type_label_hash(int raceType);

// The English.bin key a row's visible name comes from: SPEED2.EXE:0x516280
// formats "TRACK_NAME_%d" with the record's track id and hashes it. Returns the
// hash rather than the string because the caller only ever looks it up.
u32 trackdb_track_name_hash(u16 trackId);

// Builds the ring exactly as SPEED2.EXE:0x4CDED0 does: filter, wrap each
// survivor in a node carrying its locked flag and icon, then stable-sort
// unlocked rows ahead of locked ones (comparator 0x4987A0). Returns the number
// of rows written, capped at `maxRows`.
//
// `unlocks` is the career event table, which is what a row's locked flag is
// really made of -- see trackdb.c. Passing NULL marks every row unlocked, the
// short-circuit the engine's own global 0x838464 takes.
//
// One deviation, marked as such: the rows this port excludes on purpose are
// dropped here too (trackdb_port_excludes() in trackdb.c). The filter itself,
// trackdb_race_type_allows(), stays a literal transcription.
// `prog`/`progCount` is the event-progress table 0x514340 searches
// (DAT_00862840 / DAT_0086283C): the loaded career's state records, the same
// 8-byte {id, type, state} rows the save carries (#419) -- a won career event
// unlocks its tracks. NULL / 0 for a profile with no career.
int trackdb_build_rows(const TrackDb* db, int raceType, const EventDb* unlocks,
	const EventProgress* prog, int progCount, TrackRow* rows, int maxRows);
