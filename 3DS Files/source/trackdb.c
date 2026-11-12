#include "trackdb.h"
#include "bunidx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See trackdb.h for where the table lives and how the engine indexes it.
#define TRACKDB_CHUNK_TYPE 0x00034201u
#define TRACKDB_RECORD_SIZE 296 // 0x128, the stride SPEED2.EXE:0x5D3E80 uses

// Record field offsets, all confirmed against the shipped PC GLOBALB.BUN.
#define TR_OFF_REGION_NAME 0x40 // char[], "L4RA" -- see trackdb.h
#define TR_OFF_REGION 0x7C
#define TR_OFF_TRAFFIC 0x80
#define TR_OFF_P2P 0x85
#define TR_OFF_REVERSE 0x86
#define TR_OFF_EXCLUDE 0x88
#define TR_OFF_TRACKID 0x8A
#define TR_OFF_MASK 0x94
#define TR_OFF_DISTANCE 0x98
#define TR_OFF_PAR_TIME 0x9C  // two floats, kind 0 then kind 1
#define TR_OFF_PAR_SCORE 0xA4 // two ints, same pairing
#define TR_OFF_MAP_ORIGIN 0xAC // two floats: world X of the art's left edge,
                               // world Y of its top edge
#define TR_OFF_MAP_SPAN 0xB4   // world units the square art covers
#define TR_OFF_MAP_ZOOM 0xC0   // Minimap::Tick's divisor; see trackdb.h
#define TR_OFF_SEL_CENTRE 0x118 // two floats, the world map's zoom centre
#define TR_OFF_SEL_ZOOM 0x120   // float, its zoom window
#define TR_OFF_SEL_SNAP 0x124   // byte, snap rather than animate
#define TR_OFF_TRAFFIC_CARS 0xEC // eight bytes, see trackdb.h
#define TR_OFF_TRAFFIC_START_OK 0xF4 // two bytes, see trackdb.h
#define TR_OFF_CAR_CONFIG 0xF6  // s8, see trackdb.h
#define TR_OFF_TRAFFIC_START_CLEAR 0xF8 // two floats
#define TR_OFF_TRAFFIC_SPACING 0x100 // two floats
#define TR_OFF_TRAFFIC_ONCOMING 0x108 // four floats
#define TR_OFF_FORK_AFTER 0xE4  // s16, see trackdb.h

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 rd16le(const u8* p) {
	return (u16)((u32)p[0] | ((u32)p[1] << 8));
}
static float rdf32le(const u8* p) {
	u32 bits = rd32le(p);
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

bool trackdb_load(const char* bunPath, TrackDb* out) {
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

	// bChunk containers are a flat {u32 type, u32 size} list at the top level,
	// and 0x34201 is a top-level chunk in GLOBALB.BUN (verified: entry 293 of
	// 298, at file offset 0x735F00). The shared index (bunidx.h) has walked
	// those headers once already, so this is a lookup and one seek.
	long found = -1;
	u32 foundSize = 0;
	long hdrOff = 0;
	if (bunidx_find(bunPath, TRACKDB_CHUNK_TYPE, &hdrOff, &foundSize))
		found = hdrOff + 8;

	if (found < 0) {
		fclose(f);
		return false;
	}

	int count = (int)(foundSize / TRACKDB_RECORD_SIZE); // exactly the engine's rule
	if (count <= 0) {
		fclose(f);
		return false;
	}
	if (count > TRACKDB_MAX_RECORDS)
		count = TRACKDB_MAX_RECORDS;

	// Only the fields the frontend and the minimap read are kept, so the
	// resident cost is count * 44 bytes rather than count * 296.
	// The records come in one read (57 KB for the shipped 192) rather than a
	// seek and a read each; it is read again at every race start (#270).
	u8* all = (u8*)malloc((size_t)count * TRACKDB_RECORD_SIZE);
	if (!all) {
		fclose(f);
		return false;
	}
	if (fseek(f, found, SEEK_SET) != 0)
		count = 0;
	else
		count = (int)fread(all, TRACKDB_RECORD_SIZE, (size_t)count, f);
	int kept = 0;
	for (int i = 0; i < count; i++) {
		const u8* rec = all + (size_t)i * TRACKDB_RECORD_SIZE;
		TrackRec* r = &out->recs[kept++];
		r->trackId = rd16le(rec + TR_OFF_TRACKID);
		memcpy(r->regionName, rec + TR_OFF_REGION_NAME, sizeof(r->regionName) - 1);
		r->regionName[sizeof(r->regionName) - 1] = '\0';
		r->regionIcon = rec[TR_OFF_REGION];
		r->exclude = rec[TR_OFF_EXCLUDE];
		r->allowsTraffic = (rd32le(rec + TR_OFF_TRAFFIC) == 1) ? 1 : 0;
		r->allowsReverse = (rec[TR_OFF_REVERSE] == 1) ? 1 : 0;
		r->pointToPoint = rec[TR_OFF_P2P] != 0;
		r->raceTypeMask = rd32le(rec + TR_OFF_MASK);
		r->distanceM = rd32le(rec + TR_OFF_DISTANCE);
		for (int k = 0; k < 2; k++) {
			r->parTimeMs[k] = rdf32le(rec + TR_OFF_PAR_TIME + k * 4);
			r->parScore[k] = (s32)rd32le(rec + TR_OFF_PAR_SCORE + k * 4);
		}
		r->mapOriginX = rdf32le(rec + TR_OFF_MAP_ORIGIN);
		r->mapOriginY = rdf32le(rec + TR_OFF_MAP_ORIGIN + 4);
		r->mapSpan = rdf32le(rec + TR_OFF_MAP_SPAN);
		r->mapZoom = rdf32le(rec + TR_OFF_MAP_ZOOM);
		r->selCentreX = rdf32le(rec + TR_OFF_SEL_CENTRE);
		r->selCentreY = rdf32le(rec + TR_OFF_SEL_CENTRE + 4);
		r->selZoom = rdf32le(rec + TR_OFF_SEL_ZOOM);
		r->selSnap = rec[TR_OFF_SEL_SNAP] != 0;
		memcpy(r->trafficCars, rec + TR_OFF_TRAFFIC_CARS, sizeof(r->trafficCars));
		for (int k = 0; k < 2; k++) {
			r->trafficStartOk[k] = rec[TR_OFF_TRAFFIC_START_OK + k];
			r->trafficStartClear[k] = rdf32le(rec + TR_OFF_TRAFFIC_START_CLEAR + k * 4);
			r->trafficSpacing[k] = rdf32le(rec + TR_OFF_TRAFFIC_SPACING + k * 4);
		}
		r->carConfig = (s8)rec[TR_OFF_CAR_CONFIG];
		for (int k = 0; k < TRACKDB_TRAFFIC_LEVELS; k++)
			r->trafficOncoming[k] = rdf32le(rec + TR_OFF_TRAFFIC_ONCOMING + k * 4);
		r->forkAfterSec = (s16)rd16le(rec + TR_OFF_FORK_AFTER);
	}
	free(all);
	fclose(f);

	out->count = kept;
	return kept > 0;
}

const TrackRec* trackdb_find(const TrackDb* db, u16 trackId) {
	for (int i = 0; i < db->count; i++) {
		if (db->recs[i].trackId == trackId)
			return &db->recs[i];
	}
	return NULL;
}

// SPEED2.EXE:0x4987C0, Quick Race path. The engine reads the race type from
// global 0x83AAB4 (0x850088 for LAN/Online) and dispatches through the jump
// table at 0x498880; that table is transcribed literally below, including the
// three race types that can never match and the two that share Free Run's bit.
//
// The LAN/Online-only rejects (track ids 0xFA0 and 0x12C1) are deliberately
// absent: they are gated on frontend context 8/0x200, and this port only ever
// reaches context 2 (Quick Race). They are noted in trackdb.h so the omission
// reads as a scope statement rather than a lost detail.
bool trackdb_race_type_allows(const TrackRec* r, int raceType, int secondary) {
	if (!r)
		return false;
	if (r->exclude != 0)
		return false;
	if (raceType < 0 || raceType > 0xC)
		return false;

	u32 m = r->raceTypeMask;
	switch (raceType) {
	case 0x0: return (m & 0x1u) != 0;                    // Circuit  -> bit 0
	case 0x1: return ((m >> 2) & 0x1u) != 0;             // Sprint   -> bit 2
	case 0x2: return (m & 0x2u) != 0 && secondary == 0;  // Drag     -> bit 1
	case 0x3: return (m & 0x8u) != 0 && secondary == 0;  // Drift    -> bit 3
	case 0x4: return ((m >> 13) & 0x1u) != 0;            // URL Race -> bit 13
	case 0x5: return ((m >> 12) & 0x1u) != 0;            //          -> bit 12
	case 0x6: return false;
	case 0x7: return false;
	case 0x8: return ((m >> 4) & 0x1u) != 0;             // Outrun   -> bit 4
	case 0x9: return ((m >> 4) & 0x1u) != 0;             // Free Run -> bit 4
	case 0xA: return ((m >> 11) & 0x1u) != 0;            // Street X -> bit 11
	case 0xB: return false;
	case 0xC: return (m & 0x5u) != 0 && secondary == 0;  //          -> bit 0|2
	default: return false;
	}
}

// SPEED2.EXE:0x4984B0. A locked row always shows the padlock; otherwise the
// icon comes from the record's region index (+0x7C), and region 5 -- the
// records with no city region, i.e. the closed-course and drag/drift venues --
// defers to a per-race-type table (0x498574, indexed by raceType - 2).
//
// Every hash here is read straight out of the exe's own `mov eax, imm32` or
// bStringHash("...") site, not derived from a guessed name. Presence in the
// shipped data was checked by scanning the retail PC files for each hash: the
// five LOCATION_ICON_* and UI_ICON_DEBUG are in GLOBALB.BUN, the padlock and
// Track_Icon_Drag/Drift are in FrontB.lzc, and Track_Icon_Short_Track and
// Track_Icon_U_Racing_League are in no shipped file at all.
//
// Those last two are unreachable in practice, which is presumably why they were
// never packed: the region-5 arm they sit under has exactly one occupant in the
// shipped 131 records -- track 4901, a dev record -- and 4901 is also one of the
// 20 records with no TRACK_NAME_ string, so the frontend drops it before it can
// ask for an icon. Every row the player can actually be shown carries region
// 0..4 and therefore a LOCATION_ICON_* that does exist. The arm is transcribed
// anyway because it is what the exe does, not because it fires.
//
// Documented oddity, reproduced verbatim: region 0 selects UPPER_CLASS while
// the records that carry region 0 are the Suburban Hills tracks, and region 2
// selects SUBURBAN_HILLS while region 2 holds the Upper Class tracks. The two
// are crossed relative to the dev names in those same records (regions 1 =
// City Core and 4 = Airport line up). That is what the exe does; it is not a
// transcription slip here.
#define ICON_TRACK_LOCKED 0xF05704ACu
#define ICON_LOC_UPPER_CLASS 0xB5A633A0u    // "LOCATION_ICON_UPPER_CLASS"
#define ICON_LOC_CITY_CORE 0x79CEAD40u      // "LOCATION_ICON_CITY_CORE"
#define ICON_LOC_SUBURBAN_HILLS 0x4B938E3Cu // "LOCATION_ICON_SUBURBAN_HILLS"
#define ICON_LOC_INDUSTRIAL 0x4FDE6FEBu     // pack name truncated to
                                            // "LOCATION_ICON_INDUSTRIA"; the
                                            // full authored name did not
                                            // survive in the exe's strings
#define ICON_LOC_AIRPORT 0x6BA31820u        // "LOCATION_ICON_AIRPORT"
#define ICON_TRACK_DRAG 0x9574FA59u         // "Track_Icon_Drag"
#define ICON_TRACK_DRIFT 0x441467B4u        // "Track_Icon_Drift"
#define ICON_TRACK_URL 0xC5FA3835u          // "Track_Icon_U_Racing_League"
#define ICON_TRACK_SHORT 0x2B36255Fu        // "Track_Icon_Short_Track"
#define ICON_DEBUG 0x74CE8C0Bu              // "UI_ICON_DEBUG"

u32 trackdb_row_icon_hash(const TrackRec* r, int raceType, bool locked) {
	if (locked)
		return ICON_TRACK_LOCKED;
	if (!r)
		return ICON_DEBUG;
	switch (r->regionIcon) {
	case 0: return ICON_LOC_UPPER_CLASS;
	case 1: return ICON_LOC_CITY_CORE;
	case 2: return ICON_LOC_SUBURBAN_HILLS;
	case 3: return ICON_LOC_INDUSTRIAL;
	case 4: return ICON_LOC_AIRPORT;
	case 5:
		// Table 0x498574, index raceType - 2, bounds-checked unsigned -- so
		// race types 0 and 1 underflow past the table and take the default.
		switch (raceType) {
		case 0x2: return ICON_TRACK_DRAG;
		case 0x3: return ICON_TRACK_DRIFT;
		case 0x4: return ICON_TRACK_URL;
		case 0xA: return ICON_TRACK_SHORT;
		default: return ICON_DEBUG;
		}
	default: return ICON_DEBUG;
	}
}

// SPEED2.EXE:0x4988C0 with its jump table at 0x498970 -- the English.bin key
// the RACE_TYPE info field is filled from. Race types 5..8 all map back to
// "Circuit", which is the table's own default arm, not a fallback added here.
u32 trackdb_race_type_label_hash(int raceType) {
	switch (raceType) {
	case 0x1: return 0xDCB9F861u; // "Sprint"
	case 0x2: return 0x35102EDFu; // "Drag"
	case 0x3: return 0xD7162CFAu; // "Drift"
	case 0x4: return 0x75F903F4u; // "URL Race"
	case 0x9: return 0x6F2421B7u; // "Free Run"
	case 0xA: return 0x30DA61E1u; // "Street X"
	default: return 0x93FDEE74u;  // "Circuit" -- types 0 and 5..8
	}
}

// SPEED2.EXE:0x516280 -- the row-name lookup the populate pass runs per row.
// It sprintf's "TRACK_NAME_%d" (or "REVERSE_TRACK_NAME_%d" when the node's
// reverse flag is set, which the Quick Race populate pass never sets) and hands
// the result to the language table's hashing lookup.
//
// The hash is bStringHash, the same djb2 variant fng.c already ports for
// texture-pack names (SPEED2.EXE:0x50B9C0): seed 0xFFFFFFFF, h = c + h * 0x21.
// The engine's uppercase fold is a no-op on these keys -- they are built from a
// literal that is already uppercase and a decimal id -- so it is not repeated
// here, and this stays a hash of exactly the bytes the engine hashes.
u32 trackdb_track_name_hash(u16 trackId) {
	char key[32];
	snprintf(key, sizeof(key), "TRACK_NAME_%d", (int)trackId);
	u32 h = 0xFFFFFFFFu;
	for (const unsigned char* p = (const unsigned char*)key; *p; p++)
		h = (u32)*p + h * 0x21u;
	return h;
}

// Records this port keeps out of the ring by choice, not because the engine
// does. This is the record-level companion to MenuItem::excluded in main.c and
// it is deliberately separate from trackdb_race_type_allows(), which stays a
// literal transcription of 0x4987C0 -- the engine's own "never list this"
// switch is the +0x88 exclude byte that function already honours (set on four
// records: the three SW tune loops and the dyno pad).
//
// Ids 4090..4099 are the ten "XBL Outrun #1..#10" records: Xbox Live outrun
// spawn points, not track choices. All ten resolve through TRACK_NAME_40xx to
// the same string, "Outrun", so they could only ever be shown as ten
// identically labelled rows -- and they are the excluded Outrun mode's own
// content, so they go out with it.
//
// Their mask is 0x0070, of which only bit 4 is a bit any Quick Race race type
// tests: Outrun's and Free Run's shared bit. With Outrun excluded and Free
// Run's flow edge going straight to car select, nothing reaches them today, so
// this is a guard rather than a live filter -- it is here so they cannot
// reappear when a later layer gives bit 4 a track screen again. Nothing about
// the records is discarded either: they are still loaded, still counted, and
// still carry their real mask, region and id.
static bool trackdb_port_excludes(const TrackRec* r) {
	return r->trackId >= 4090 && r->trackId <= 4099;
}

int trackdb_build_rows(const TrackDb* db, int raceType, const EventDb* unlocks,
		const EventProgress* prog, int progCount, TrackRow* rows, int maxRows) {
	if (!db || !rows || maxRows <= 0)
		return 0;

	// SPEED2.EXE:0x4CDED0 walks the record array in file order, keeps every
	// record 0x4987C0 accepts, and wraps each in a 0x30-byte node whose +8 is
	// the locked flag from 0x5156F0(ctx = 1, trackId) (SPEED2.EXE:0x4B4C10).
	// It then sorts the list with comparator 0x4987A0, which orders unlocked
	// rows ahead of locked ones. Emitting in two stable passes over the records
	// -- unlocked first, then locked -- is that same ordering, and keeps the
	// record-file order the engine's list build gives each half, with no temp
	// buffer and no unstable swap.
	//
	// Where the locked flag comes from. 0x4B4C10 picks an unlock context from
	// the frontend global 0x850078 -- 0x1F8 for LAN and Online, 1 for everything
	// else, so 1 here -- and calls 0x5156F0(context, trackId). That opens with a
	// short-circuit on the global byte 0x838464, then routes both the context-1
	// and context-0x1F8 arms to 0x5143C0(trackId), the real query: walk every
	// event whose track list mentions the track and accept the track as soon as
	// one of them passes 0x514340. eventdb.c is that query.
	//
	// 0x838464 is not "the loaded state" an earlier revision of this comment
	// assumed. It is BSS -- .data's raw size is 0x35000 and the address is
	// 0x7A464 into the section -- and it has twelve read sites in .text and no
	// write site at all, in any direct store form, so it is zero for the whole
	// life of the process and 0x5156F0 never short-circuits. The progress table
	// 0x514340 falls back on is NOT dead, whatever an earlier revision said
	// (#419, a tester: career wins never unlocked Quick Race tracks): no direct
	// store names it because it is filled through a pointer -- it is the
	// career's state-record table, the save's 8-byte {id, type, state} rows, so
	// the caller hands the loaded profile's.
	//
	// So a retail profile that has finished no events has exactly the tracks the
	// event table marks with flag bit 0x2, and that is what `unlocks` yields.
	// Passing NULL is still the 0x838464 path, for a caller that wants it.
	int n = 0;
	for (int pass = 0; pass < 2 && n < maxRows; pass++) {
		bool wantLocked = (pass == 1);
		for (int i = 0; i < db->count && n < maxRows; i++) {
			const TrackRec* r = &db->recs[i];
			bool locked = unlocks &&
				!eventdb_track_unlocked(unlocks, r->trackId, prog, progCount);
			if (locked != wantLocked)
				continue;
			if (!trackdb_race_type_allows(r, raceType, 0))
				continue;
			if (trackdb_port_excludes(r))
				continue;
			rows[n].trackId = r->trackId;
			rows[n].locked = locked;
			rows[n].iconHash = trackdb_row_icon_hash(r, raceType, locked);
			rows[n].distanceM = r->distanceM;
			// kind 0: SPEED2.EXE:0x4B4C10 zero-fills node+0x10 and the
			// track-select path never writes it -- see trackdb.h.
			rows[n].bestTime = trackdb_best_time_ticks(r, 0);
			rows[n].bestScore = trackdb_best_score(r, 0);
			n++;
		}
	}

	return n;
}

// SPEED2.EXE:0x502AA0 / 0x502C50, miss path only -- see trackdb.h for why the
// profile table itself is never consulted here.
s32 trackdb_best_time_ticks(const TrackRec* r, int kind) {
	float ms = r->parTimeMs[kind ? 1 : 0];
	// 0x502C22: FMUL 0.001 / FMUL 4000.0 / FADD 0.5, then the ftol at
	// 0x75BED4. The two multiplies are the engine's own, kept apart rather
	// than folded to x4 so the constants stay recognisable.
	return (s32)(ms * 0.001f * 4000.0f + 0.5f);
}

s32 trackdb_best_score(const TrackRec* r, int kind) {
	return r->parScore[kind ? 1 : 0];
}
