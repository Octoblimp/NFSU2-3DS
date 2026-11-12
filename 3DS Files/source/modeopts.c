#include "modeopts.h"
#include <string.h>

#include "lanlobby.h"

// ---------------------------------------------------------------------------
// English.bin keys
//
// Every hash below was read straight out of the row class it belongs to and
// then looked up in the shipped LANGUAGES/English.bin; the comment is that
// lookup's result, not a guess at what the row ought to say.
// ---------------------------------------------------------------------------

#define LBL_TRACK_DIRECTION 0xA88FFEB4u // "Track Direction"  refresh 0x4B4150
#define LBL_LAP_KO 0x7FD73849u          // "Lap KO"           refresh 0x4B3FD0
#define LBL_LAPS 0x48494E83u            // "Laps"             refresh 0x4B3DD0
#define LBL_TRAFFIC 0xEB9DFC09u         // "Traffic Level"    refresh 0x4B3B30
#define LBL_OPPONENTS 0x3384A679u       // "Opponents"        refresh 0x4B38C0
#define LBL_DIFFICULTY 0x4D156786u      // "Difficulty"       refresh 0x4B39C0
#define LBL_CATCH_UP 0x8B8E913Au        // "Catch Up"         refresh 0x4B3A60
#define LBL_POINTS_TO_WIN 0x81CAA8E6u   // "Number of Points to Win:"  0x4B3C00
#define LBL_TIME_LIMIT 0xFE407A18u      // "Time Limit"       refresh 0x4B3CC0

#define VAL_ON 0x0000063Cu           // "On"
#define VAL_OFF 0x0000CCFAu          // "Off"
#define VAL_FORWARD 0x9E4C4648u      // "Forward"
#define VAL_REVERSE 0x22F254AFu      // "Reverse"
#define VAL_EASY 0xDBBB4E8Fu         // "Easy"
#define VAL_MEDIUM 0xCAB262DEu       // "Medium"
#define VAL_HARD 0xDBBCF37Cu         // "Hard"
#define VAL_TRAFFIC_NONE 0x896BCB4Du // "None"
#define VAL_TRAFFIC_MIN 0xB6BD9C95u  // "Minimum"
#define VAL_TRAFFIC_MOD 0x907C555Eu  // "Moderate"
#define VAL_TRAFFIC_MAX 0xB672F15Au  // "Maximum"
#define VAL_UNLIMITED 0x2F8344EAu    // "Unlimited"
#define VAL_TIME_NONE 0x53B157FCu    // "None"
#define VAL_MINUTES 0x542110C3u      // "Minutes"
// The LAN rows' (FUN_004b41c0's row classes, TASKS #321b).
#define VAL_YES 0x0000F770u          // "Yes"
#define VAL_NO 0x0000061Cu           // "No"
#define VAL_EVENLY 0xFABFC6DCu       // "Evenly Matched"
#define VAL_RESTRICT_NONE 0xFAC4999Eu // "None"
#define VAL_REQUIRED 0xBB298A80u     // "REQUIRED"

#define LBL_LAN_PROTECTED 0x744A80A7u   // "Password protected?"
#define LBL_LAN_PASSWORD 0x9953843Bu    // "Enter password"
#define LBL_LAN_CONFIRM 0x97CB518Bu     // "Confirm password"
#define LBL_LAN_MAX_PLAYERS 0xA40967B0u // "Maximum Players"
#define LBL_LAN_COLLISION 0x7712E575u   // "Collision Detection"
#define LBL_LAN_RESTRICTION 0x96A1E80Fu // "Car Performance Restrictions"
#define LBL_LAN_RACE_BREAKER 0xB82A976Eu // "Race Breaker"

// The two value tables the Outrun rows index, .rdata as shipped.
// 0x7992CC, 7 entries; index 6 is 0xFF and prints as "Unlimited" instead.
static const u8 kPointsToWin[7] = { 10, 20, 30, 40, 50, 100, 255 };
// 0x7992D4, 5 entries; index 0 is 0 and prints as "None" instead.
static const u8 kTimeLimit[5] = { 0, 2, 5, 8, 10 };

// ---------------------------------------------------------------------------
// The settings object -- SPEED2.EXE:0x83AAB4
// ---------------------------------------------------------------------------

// SPEED2.EXE:0x515800. The object lives in BSS, so this function is the only
// record of the shipped defaults anywhere in the executable. Transcribed write
// for write; the address of each override is in its comment.
void raceset_init(RaceSettings* s) {
	memset(s, 0, sizeof(*s));
	s->raceType = 0; // 0x515809

	// 0x515820..0x51583E: thirteen identical 20-byte blocks.
	for (int i = 0; i < RACESET_MODES; i++) {
		s->mode[i].minOpponents = 1;
		s->mode[i].opponents = 3;
		s->mode[i].difficulty = 1;   // Medium
		s->mode[i].unk03 = 1;
		s->mode[i].trafficLevel = 1; // Minimum
		s->mode[i].catchUp = 1;      // On
		s->mode[i].unk06 = 0;
		// 0x515800 never writes a block's lap count, so the memset above would
		// leave every record on 0 laps. Retail's records are TrackSettings, which
		// start out as TrackSettings::Default (Speed.elf 0x80130D94, called per
		// block from the UserProfile ctor 0x801319F0 before the Quick Race
		// defaults): track 0, 3 laps, forward. The PC save carries the same --
		// its Quick Race object (payload 0xC5A7) holds 3 laps in every block the
		// overrides below leave alone.
		s->mode[i].record.laps = 3;
	}

	s->mode[1].record.laps = 1;          // 0x515845  Sprint
	s->mode[2].record.laps = 1;          // 0x515849  Drag
	s->mode[5].opponents = 1;            // 0x51584D
	s->mode[0].record.trackId = 0x0FB5;  // 0x515852  Circuit, track 4021
	s->mode[3].record.laps = 3;          // 0x515858  Drift
	s->mode[3].trafficLevel = 2;         // 0x51585C  Drift, Moderate
	s->mode[9].record.laps = 0;          // 0x515860  Free Run
	s->mode[9].opponents = 0;            // 0x515869  Free Run
	s->mode[5].difficulty = 0;           // 0x515870
	s->mode[10].difficulty = 0;          // 0x515874  Street X, Easy
	s->mode[10].record.trackId = 0x11F9; // 0x51587B  Street X, track 4601
	s->mode[10].trafficLevel = 0;        // 0x515884  Street X, None
	s->mode[10].record.laps = 10;        // 0x51588B  Street X
	s->mode[4].opponents = 5;            // 0x515894  URL

	s->pointsToWinIdx = 2; // 0x515898 -> kPointsToWin[2] == 30
	s->timeLimitIdx = 0;   // 0x5158A2 -> "None"

	// 0x515954 tails into 0x515750, which initialises the race-type-0xC party
	// playlist at +0x100. This port has no online playlist, and the 0xC block
	// here is the plain 13th block, so there is nothing extra to transcribe:
	// every value 0x515750 writes for a field this struct models (min 1,
	// opponents 3, difficulty 1, traffic 1, catch up 1, laps 3) is already what
	// the loop above put in mode[12], bar its per-queued-race records.
}

// The blocks are indexed by the object's own race type. A type outside the
// table cannot happen through any path this port has (the dispatcher bounds it
// at 0xA first), but the accessors are the load-bearing ones, so they clamp.
static int blockIndex(const RaceSettings* s) {
	int t = s->raceType;
	if (t < 0 || t >= RACESET_MODES)
		return 0;
	return t;
}

u8 raceset_min_opponents(const RaceSettings* s) { return s->mode[blockIndex(s)].minOpponents; }
u8 raceset_opponents(const RaceSettings* s) { return s->mode[blockIndex(s)].opponents; }
u8 raceset_difficulty(const RaceSettings* s) { return s->mode[blockIndex(s)].difficulty; }
u8 raceset_traffic(const RaceSettings* s) { return s->mode[blockIndex(s)].trafficLevel; }
u8 raceset_catch_up(const RaceSettings* s) { return s->mode[blockIndex(s)].catchUp; }
RaceRecord* raceset_record(RaceSettings* s) { return &s->mode[blockIndex(s)].record; }

const RaceRecord* raceset_record_const(const RaceSettings* s) {
	return &s->mode[blockIndex(s)].record;
}

// ---------------------------------------------------------------------------
// Row building -- SPEED2.EXE:0x4E2C70 and the eight builders behind it
// ---------------------------------------------------------------------------

typedef struct {
	ModeOptRow* rows;
	int n;
	int max;
} RowSink;

static void addRow(RowSink* sink, ModeOptKind kind, u32 labelHash) {
	if (sink->n >= sink->max)
		return;
	sink->rows[sink->n].kind = kind;
	sink->rows[sink->n].labelHash = labelHash;
	sink->n++;
}

// SPEED2.EXE:0x4B3793 -- the Opponents row's own constructor, not its handler.
// Creating the row on the quick-race drag strip 4402 with more than two
// opponents already selected writes the count back down to two before anything
// is drawn, so a car count carried over from another track cannot survive into
// a race the strip cannot hold. modeopts_adjust() enforces the same ceiling
// from 0x4B3837; this is the other half of it.
static void addOpponents(RowSink* sink, RaceSettings* s, int ctx) {
	if (ctx == MODEOPTS_CTX_QUICKRACE && s->raceType == 2 &&
			raceset_record(s)->trackId == 0x1132 && raceset_opponents(s) > 2)
		s->mode[blockIndex(s)].opponents = 2;
	addRow(sink, MODEOPT_OPPONENTS, LBL_OPPONENTS);
}

// The Track Direction row every builder opens with. The engine allocates it
// (new(0x54) -> 0x4B40B0) only when the selected track record's reverse flag is
// 1; every builder but the circuit's (0x4CCF60) -- sprint 0x4CD2D0, drag
// 0x4CD530, drift 0x4CD7A0, URL / Street X 0x4B47B0, type 7 0x4CDB40, Free Run
// 0x4B4A10 -- also asks the unlock check 0x5156F0(1, record +0x8A, 1, 0)
// (#517), `gated` here.
static void addTrackDirection(RowSink* sink, const ModeOptTrack* trk, bool gated) {
	if (trk && trk->allowsReverse && (!gated || trk->unlocked))
		addRow(sink, MODEOPT_TRACK_DIRECTION, LBL_TRACK_DIRECTION);
}

// SPEED2.EXE:0x4CCF60 -- race type 0, Circuit.
static void buildCircuit(RaceSettings* s, int ctx, const ModeOptTrack* trk, RowSink* sink) {
	// 0x4CCF99: split-screen cannot run Lap KO, so a flag left over from a
	// previous quick race is cleared before the list is built.
	// PORT (#419, the user 2026-11-07: "DO NOT IMPLEMENT LAP KO AND HIDE IT,
	// IT IS NOT ENABLED IN RETAIL"): the shipped game never offers Lap KO, so
	// the flag is cleared in every context -- a save or an older build that
	// set it would otherwise lock the Laps row (modeopts_row_disabled) -- and
	// the row 0x4CD03B allocates is not listed.
	if (raceset_record(s)->lapKO)
		raceset_record(s)->lapKO = 0;

	addTrackDirection(sink, trk, false);              // 0x4CCFD9
	addRow(sink, MODEOPT_LAPS, LBL_LAPS);             // 0x4CD0A0 / split arm
	addRow(sink, MODEOPT_TRAFFIC, LBL_TRAFFIC);       // inline vtable 0x791458
	if (ctx != MODEOPTS_CTX_SPLITSCREEN) {
		addOpponents(sink, s, ctx);
		addRow(sink, MODEOPT_DIFFICULTY, LBL_DIFFICULTY);
	}
	addRow(sink, MODEOPT_CATCH_UP, LBL_CATCH_UP);     // inline vtable 0x791410
}

// SPEED2.EXE:0x4CD2D0 -- race type 1, Sprint.
static void buildSprint(RaceSettings* s, int ctx, const ModeOptTrack* trk, RowSink* sink) {
	(void)s;
	addTrackDirection(sink, trk, true);
	addRow(sink, MODEOPT_TRAFFIC, LBL_TRAFFIC);
	if (ctx != MODEOPTS_CTX_SPLITSCREEN) {
		addOpponents(sink, s, ctx);
		addRow(sink, MODEOPT_DIFFICULTY, LBL_DIFFICULTY);
	}
	addRow(sink, MODEOPT_CATCH_UP, LBL_CATCH_UP);
}

// SPEED2.EXE:0x4CD530 -- race type 2, Drag.
static void buildDrag(RaceSettings* s, int ctx, const ModeOptTrack* trk, RowSink* sink) {
	(void)s;
	addTrackDirection(sink, trk, true);
	// 0x4CD5F0: the drag strips 4211..4223 get no Traffic row.
	if (!trk || trk->trackId < 0x1073 || trk->trackId > 0x107F)
		addRow(sink, MODEOPT_TRAFFIC, LBL_TRAFFIC);
	if (ctx != MODEOPTS_CTX_SPLITSCREEN) {
		addOpponents(sink, s, ctx);
		addRow(sink, MODEOPT_DIFFICULTY, LBL_DIFFICULTY);
	}
	addRow(sink, MODEOPT_CATCH_UP, LBL_CATCH_UP);
}

// SPEED2.EXE:0x4CD7A0 -- race type 3, Drift. The only builder that ends on
// Traffic instead of Catch Up, and the only one whose Traffic row is gated:
// 0x4CD7EE tests the track record's +0x80 flag before allocating 0x4B3AA0.
static void buildDrift(RaceSettings* s, int ctx, const ModeOptTrack* trk, RowSink* sink) {
	(void)s;
	addTrackDirection(sink, trk, true);
	addRow(sink, MODEOPT_LAPS, LBL_LAPS);
	if (ctx != MODEOPTS_CTX_SPLITSCREEN) {
		addOpponents(sink, s, ctx);
		addRow(sink, MODEOPT_DIFFICULTY, LBL_DIFFICULTY);
	}
	if (trk && trk->allowsTraffic)
		addRow(sink, MODEOPT_TRAFFIC, LBL_TRAFFIC);
}

// SPEED2.EXE:0x4B47B0 -- race types 4 and 5 (URL) and 0xA (Street X).
static void buildUrl(RaceSettings* s, int ctx, const ModeOptTrack* trk, RowSink* sink) {
	(void)s;
	addTrackDirection(sink, trk, true);
	addRow(sink, MODEOPT_LAPS, LBL_LAPS);
	if (ctx != MODEOPTS_CTX_SPLITSCREEN) {
		addOpponents(sink, s, ctx);
		addRow(sink, MODEOPT_DIFFICULTY, LBL_DIFFICULTY);
	}
	addRow(sink, MODEOPT_CATCH_UP, LBL_CATCH_UP);
}

// SPEED2.EXE:0x4CDB40 -- race type 7. No lap or traffic row.
static void buildType7(RaceSettings* s, int ctx, const ModeOptTrack* trk, RowSink* sink) {
	(void)s;
	addTrackDirection(sink, trk, true);
	if (ctx != MODEOPTS_CTX_SPLITSCREEN) {
		addOpponents(sink, s, ctx);
		addRow(sink, MODEOPT_DIFFICULTY, LBL_DIFFICULTY);
	}
	addRow(sink, MODEOPT_CATCH_UP, LBL_CATCH_UP);
}

// SPEED2.EXE:0x4CDA20 -- race type 8, Outrun. Two rows, neither of them a track
// or opponent setting: this is the only mode whose options come out of the
// object's +0x108/+0x10C indices.
static void buildOutrun(RaceSettings* s, int ctx, const ModeOptTrack* trk, RowSink* sink) {
	(void)s;
	(void)ctx;
	(void)trk;
	addRow(sink, MODEOPT_POINTS_TO_WIN, LBL_POINTS_TO_WIN); // inline 0x7914A0
	addRow(sink, MODEOPT_TIME_LIMIT, LBL_TIME_LIMIT);       // inline 0x7914E8
}

// SPEED2.EXE:0x4B4A10 -- race type 9, Free Run. No opponents, so no difficulty
// or catch-up either.
static void buildFreeRun(RaceSettings* s, int ctx, const ModeOptTrack* trk, RowSink* sink) {
	(void)s;
	(void)ctx;
	addTrackDirection(sink, trk, true);
	addRow(sink, MODEOPT_TRAFFIC, LBL_TRAFFIC);
}

// SPEED2.EXE:0x4E2C70. The LAN/Online arms (context 8 and 0x200) read the race
// type from 0x850088 and dispatch through table 0x4E2D94 instead; this port has
// neither, so only the 0x4E2D68 table is transcribed. Race type 6 has no arm in
// that table -- it falls through to the shared tail with no rows of its own.
int modeopts_build_rows(RaceSettings* s, int ctx, const ModeOptTrack* trk,
		ModeOptRow* rows, int maxRows) {
	RowSink sink = { rows, 0, maxRows };
	if (!s || !rows || maxRows <= 0)
		return 0;
	if (s->raceType < 0 || s->raceType > 0xA) // 0x4E2C9B bounds the index
		return 0;

	switch (s->raceType) {
	case 0x0: buildCircuit(s, ctx, trk, &sink); break;
	case 0x1: buildSprint(s, ctx, trk, &sink); break;
	case 0x2: buildDrag(s, ctx, trk, &sink); break;
	case 0x3: buildDrift(s, ctx, trk, &sink); break;
	case 0x4: buildUrl(s, ctx, trk, &sink); break;
	case 0x5: buildUrl(s, ctx, trk, &sink); break;
	case 0x6: break;
	case 0x7: buildType7(s, ctx, trk, &sink); break;
	case 0x8: buildOutrun(s, ctx, trk, &sink); break;
	case 0x9: buildFreeRun(s, ctx, trk, &sink); break;
	case 0xA: buildUrl(s, ctx, trk, &sink); break;
	default: break;
	}
	// Every arm then falls into the shared tail 0x4B41C0. Its three password
	// rows hide themselves whenever 0x8500C8 is 0, and its points/time rows are
	// built only when 0x850088 == 8, so outside a hosted online game the tail
	// contributes nothing visible. Nothing to add here.
	return sink.n;
}

// ---------------------------------------------------------------------------
// Row values -- vtable slot 3 (refresh) of each row class
// ---------------------------------------------------------------------------

// SPEED2.EXE:0x4B3DD0's tail (0x4B3E02..0x4B3E32). Every refresh writes its
// label and value first, and the Laps row then compares the settings object's
// Lap KO flag against its own +0x28 "enabled" byte: with Lap KO on it takes
// vtable slot +0x20 (0x51E3E0 -- clears +0x28 and plays the row widgets'
// "Disable" script), and with Lap KO off slot +0x1C (0x51E270 -- sets +0x28 and
// plays "UnHighlight"). That is the whole of it: switching Lap KO on forces a
// three-lap race (0x4B406A), so the Laps row stops being adjustable. No other
// row class has this tail -- the eight others end at their second 0x50C900.
bool modeopts_row_disabled(const RaceSettings* s, ModeOptKind kind) {
	if (kind != MODEOPT_LAPS)
		return false;
	return raceset_record_const(s)->lapKO != 0;
}

void modeopts_row_value(const RaceSettings* s, ModeOptKind kind, ModeOptValue* out) {
	memset(out, 0, sizeof(*out));
	const RaceRecord* rec = raceset_record_const(s);

	switch (kind) {
	case MODEOPT_TRACK_DIRECTION: // 0x4B4150
		out->valueHash = (rec->direction == 0) ? VAL_FORWARD : VAL_REVERSE;
		break;

	case MODEOPT_LAP_KO: // 0x4B3FD0
		out->valueHash = rec->lapKO ? VAL_ON : VAL_OFF;
		break;

	case MODEOPT_LAPS: // 0x4B3DD0, format "%d" (0x7845D4)
		out->isNumber = true;
		out->number = rec->laps;
		break;

	case MODEOPT_TRAFFIC: { // 0x4B3B30, jump table 0x4B3B84
		static const u32 kTraffic[4] = {
			VAL_TRAFFIC_NONE, VAL_TRAFFIC_MIN, VAL_TRAFFIC_MOD, VAL_TRAFFIC_MAX
		};
		u8 v = raceset_traffic(s);
		out->valueHash = kTraffic[v < 4 ? v : 0];
		break;
	}

	case MODEOPT_OPPONENTS: // 0x4B38C0, format "%d"
		out->isNumber = true;
		out->number = raceset_opponents(s);
		break;

	case MODEOPT_DIFFICULTY: { // 0x4B39C0
		static const u32 kDiff[3] = { VAL_EASY, VAL_MEDIUM, VAL_HARD };
		u8 v = raceset_difficulty(s);
		out->valueHash = kDiff[v < 3 ? v : 0];
		break;
	}

	case MODEOPT_CATCH_UP: // 0x4B3A60
		out->valueHash = raceset_catch_up(s) ? VAL_ON : VAL_OFF;
		break;

	case MODEOPT_POINTS_TO_WIN: // 0x4B3C00
		if (s->pointsToWinIdx == 6) {
			out->valueHash = VAL_UNLIMITED;
		} else {
			int i = s->pointsToWinIdx;
			out->isNumber = true;
			out->number = kPointsToWin[(i >= 0 && i < 7) ? i : 0];
		}
		break;

	case MODEOPT_TIME_LIMIT: // 0x4B3CC0, format "%d %s" (0x787ABC)
		if (s->timeLimitIdx == 0) {
			out->valueHash = VAL_TIME_NONE;
		} else {
			int i = s->timeLimitIdx;
			out->isNumber = true;
			out->number = kTimeLimit[(i >= 0 && i < 5) ? i : 0];
			out->suffixHash = VAL_MINUTES;
		}
		break;

	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Change handlers -- vtable slot 1 of each row class
//
// The engine drives these with two message hashes, LEFT 0x9120409E and RIGHT
// 0xB5971BF1, and every handler ends by calling its own refresh (vtable +0x40)
// and redraw (+0x0C). Here the caller re-reads the value instead, which is the
// same thing without the vtable.
// ---------------------------------------------------------------------------

void modeopts_adjust(RaceSettings* s, int ctx, ModeOptKind kind, bool right) {
	RaceRecord* rec = raceset_record(s);
	RaceModeBlock* blk = &s->mode[blockIndex(s)];

	switch (kind) {
	case MODEOPT_TRACK_DIRECTION: // 0x4B40E0 -- toggles either way
		rec->direction = (rec->direction != 0) ? 0u : 1u;
		break;

	case MODEOPT_LAP_KO: // 0x4B4020 -- toggles either way
		rec->lapKO = rec->lapKO ? 0 : 1;
		// 0x4B406A: switching Lap KO on forces a three-lap, three-opponent
		// race, then broadcasts 0x92B703B5 so the Laps and Opponents rows
		// refresh too.
		if (rec->lapKO) {
			rec->laps = 3;
			blk->opponents = 3;
		}
		break;

	case MODEOPT_LAPS: { // 0x4B3D60 -- 1..10, wrapping
		int v = rec->laps;
		if (!right) {
			v--;
			if (v < 1)
				v = 10;
		} else {
			v++;
			if (v > 10)
				v = 1;
		}
		rec->laps = (u16)v;
		break;
	}

	case MODEOPT_TRAFFIC: { // 0x4B3AD0 -- 0..3, wrapping
		int v = raceset_traffic(s);
		if (!right) {
			v--;
			if (v < 0)
				v = 3;
		} else {
			v++;
			if (v > 3)
				v = 0;
		}
		blk->trafficLevel = (u8)v;
		break;
	}

	case MODEOPT_OPPONENTS: { // 0x4B37F0
		int lo = raceset_min_opponents(s);   // 0x5026F0, the block's +0x00
		int v = raceset_opponents(s);        // 0x502710
		int hi = (s->raceType == 4) ? 5 : 3; // 0x4B382B: lea ecx,[ecx+ecx+3]
		// 0x4B3837: quick race on drag strip 4402 tops out two cars lower.
		if (ctx == MODEOPTS_CTX_QUICKRACE && s->raceType == 2 && rec->trackId == 0x1132)
			hi = 2;
		// 0x4B385B: quick race may run with no opponents at all; every other
		// context needs at least one.
		int floorV = (ctx == MODEOPTS_CTX_QUICKRACE) ? 0 : 1;
		int top = hi - lo + 1;
		if (!right) {
			v--;
			if (v < floorV)
				v = top;
		} else {
			v++;
			if (v > top)
				v = floorV;
		}
		blk->opponents = (u8)v; // 0x502960
		break;
	}

	case MODEOPT_DIFFICULTY: { // 0x4B3960 -- 0..2, wrapping
		int v = raceset_difficulty(s);
		if (!right) {
			v--;
			if (v < 0)
				v = 2;
		} else {
			v++;
			if (v > 2)
				v = 0;
		}
		blk->difficulty = (u8)v;
		break;
	}

	case MODEOPT_CATCH_UP: // 0x4B3A10 -- toggles either way
		blk->catchUp = raceset_catch_up(s) ? 0 : 1;
		break;

	case MODEOPT_POINTS_TO_WIN: { // 0x4B3BA0 -- 0..6, wrapping
		int v = s->pointsToWinIdx;
		if (!right) {
			v--;
			if (v < 0)
				v = 6;
		} else {
			v++;
			if (v > 6)
				v = 0;
		}
		s->pointsToWinIdx = v;
		break;
	}

	case MODEOPT_TIME_LIMIT: { // 0x4B3C60 -- 0..4, wrapping
		int v = s->timeLimitIdx;
		if (!right) {
			v--;
			if (v < 0)
				v = 4;
		} else {
			v++;
			if (v > 4)
				v = 0;
		}
		s->timeLimitIdx = v;
		break;
	}

	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// The value-string key table, for callers that resolve English.bin once.

static const u32 kValueHashes[MODEOPTS_VALUE_HASH_COUNT] = {
	VAL_ON, VAL_OFF, VAL_FORWARD, VAL_REVERSE, VAL_EASY, VAL_MEDIUM, VAL_HARD,
	VAL_TRAFFIC_NONE, VAL_TRAFFIC_MIN, VAL_TRAFFIC_MOD, VAL_TRAFFIC_MAX,
	VAL_UNLIMITED, VAL_TIME_NONE, VAL_MINUTES,
	VAL_YES, VAL_NO, VAL_EVENLY, VAL_RESTRICT_NONE, VAL_REQUIRED,
};

const u32* modeopts_value_hashes(void) {
	return kValueHashes;
}

int modeopts_value_index(u32 hash) {
	for (int i = 0; i < MODEOPTS_VALUE_HASH_COUNT; i++) {
		if (kValueHashes[i] == hash)
			return i;
	}
	return -1;
}

// ---------------------------------------------------------------------------
// The LAN create-game rows -- FUN_004b41c0 in context 0x200 (TASKS #321b)
// ---------------------------------------------------------------------------

static int wrapStep(int v, int lo, int hi, bool right) {
	v += right ? 1 : -1;
	if (v < lo)
		v = hi;
	if (v > hi)
		v = lo;
	return v;
}

void modeopts_lan_init(ModeOptsLan* l, u8 mode, u16 track, bool pointToPoint,
		bool allowsReverse) {
	memset(l, 0, sizeof(*l));
	l->mode = mode;
	l->track = track;
	l->pointToPoint = pointToPoint;
	l->allowsReverse = allowsReverse && mode != 8;
	int lo, hi;
	lanlobby_lap_range(mode, pointToPoint, &lo, &hi);
	l->laps = (u8)lo;
	l->pointsIdx = 2; // 0x515898, as the QR rows: 30 points
	// PORT (the user 2026-11-09, "six in every mode"): Max Players 2..6 for
	// every race type; the PC's are 2..4 (URL 2..6).
	l->maxPlayers = LAN_CONSOLES;
	l->collision = 1;
}

static void lanAdd(ModeOptRow* rows, int maxRows, int* n, ModeOptKind kind, u32 label) {
	if (*n >= maxRows)
		return;
	rows[*n].kind = kind;
	rows[*n].labelHash = label;
	(*n)++;
}

int modeopts_lan_build_rows(ModeOptsLan* l, ModeOptRow* rows, int maxRows) {
	int n = 0;
	if (!l || !rows || maxRows <= 0)
		return 0;
	lanAdd(rows, maxRows, &n, MODEOPT_LAN_PROTECTED, LBL_LAN_PROTECTED);
	lanAdd(rows, maxRows, &n, MODEOPT_LAN_PASSWORD, LBL_LAN_PASSWORD);
	lanAdd(rows, maxRows, &n, MODEOPT_LAN_CONFIRM, LBL_LAN_CONFIRM);
	if (l->mode == 8) {                                   // 0x4B42B4
		lanAdd(rows, maxRows, &n, MODEOPT_LAN_POINTS_TO_WIN, LBL_POINTS_TO_WIN);
		lanAdd(rows, maxRows, &n, MODEOPT_LAN_TIME_LIMIT, LBL_TIME_LIMIT);
	}
	// 0x4B4340: Track Direction only off Outrun, on a track with a reverse
	// layout.
	if (l->mode == 8 || !l->allowsReverse)
		l->reverse = 0;
	else
		lanAdd(rows, maxRows, &n, MODEOPT_LAN_DIRECTION, LBL_TRACK_DIRECTION);
	// 0x4B43A0: no Laps row for Sprint, Drag, Outrun or a point-to-point
	// Drift; those race one lap.
	if (l->mode == 1 || l->mode == 2 || l->mode == 8 || (l->mode == 3 && l->pointToPoint))
		l->laps = 1;
	else
		lanAdd(rows, maxRows, &n, MODEOPT_LAN_LAPS, LBL_LAPS);
	// Ranked: never in LAN (0x4B4410).
	lanAdd(rows, maxRows, &n, MODEOPT_LAN_MAX_PLAYERS, LBL_LAN_MAX_PLAYERS);
	lanAdd(rows, maxRows, &n, MODEOPT_LAN_COLLISION, LBL_LAN_COLLISION);
	lanAdd(rows, maxRows, &n, MODEOPT_LAN_RESTRICTION, LBL_LAN_RESTRICTION);
	if (l->mode == 3 || l->mode == 10)                    // 0x4B4500
		l->raceBreaker = 0;
	else
		lanAdd(rows, maxRows, &n, MODEOPT_LAN_RACE_BREAKER, LBL_LAN_RACE_BREAKER);
	// Private Room: online only (0x4B4560).
	return n;
}

static void lanStars(const char* pw, char* out, size_t cap) {
	size_t k = strlen(pw);
	if (k >= cap)
		k = cap - 1;
	memset(out, '*', k);
	out[k] = '\0';
}

void modeopts_lan_row_value(const ModeOptsLan* l, ModeOptKind kind, ModeOptValue* out) {
	memset(out, 0, sizeof(*out));
	switch (kind) {
	case MODEOPT_LAN_PROTECTED:
		out->valueHash = l->protect ? VAL_YES : VAL_NO;
		break;
	case MODEOPT_LAN_PASSWORD:
		// The stars of what was typed, or "REQUIRED" while protected and
		// empty; nothing while not protected.
		if (l->password[0])
			lanStars(l->password, out->text, sizeof(out->text));
		else if (l->protect)
			out->valueHash = VAL_REQUIRED;
		break;
	case MODEOPT_LAN_CONFIRM:
		if (l->confirm[0])
			lanStars(l->confirm, out->text, sizeof(out->text));
		else if (l->protect && l->password[0])
			out->valueHash = VAL_REQUIRED;
		break;
	case MODEOPT_LAN_POINTS_TO_WIN:
		if (l->pointsIdx == 6) {
			out->valueHash = VAL_UNLIMITED;
		} else {
			out->isNumber = true;
			out->number = kPointsToWin[l->pointsIdx < 7 ? l->pointsIdx : 0];
		}
		break;
	case MODEOPT_LAN_TIME_LIMIT:
		if (l->timeIdx == 0) {
			out->valueHash = VAL_TIME_NONE;
		} else {
			out->isNumber = true;
			out->number = kTimeLimit[l->timeIdx < 5 ? l->timeIdx : 0];
			out->suffixHash = VAL_MINUTES;
		}
		break;
	case MODEOPT_LAN_DIRECTION:
		out->valueHash = l->reverse ? VAL_REVERSE : VAL_FORWARD;
		break;
	case MODEOPT_LAN_LAPS:
	case MODEOPT_LAN_MAX_PLAYERS:
		out->isNumber = true;
		out->number = kind == MODEOPT_LAN_LAPS ? l->laps : l->maxPlayers;
		break;
	case MODEOPT_LAN_COLLISION:
		out->valueHash = l->collision ? VAL_ON : VAL_OFF;
		break;
	case MODEOPT_LAN_RESTRICTION:
		out->valueHash = l->restriction ? VAL_EVENLY : VAL_RESTRICT_NONE;
		break;
	case MODEOPT_LAN_RACE_BREAKER:
		out->valueHash = l->raceBreaker ? VAL_ON : VAL_OFF;
		break;
	default:
		break;
	}
}

bool modeopts_lan_row_disabled(const ModeOptsLan* l, ModeOptKind kind) {
	if (kind == MODEOPT_LAN_PASSWORD)
		return !l->protect;
	if (kind == MODEOPT_LAN_CONFIRM)
		return !l->protect || !l->password[0];
	return false;
}

bool modeopts_lan_row_is_text(ModeOptKind kind) {
	return kind == MODEOPT_LAN_PASSWORD || kind == MODEOPT_LAN_CONFIRM;
}

void modeopts_lan_adjust(ModeOptsLan* l, ModeOptKind kind, bool right) {
	int lo, hi;
	switch (kind) {
	case MODEOPT_LAN_PROTECTED:     // toggles either way
		l->protect = l->protect ? 0 : 1;
		if (!l->protect) {
			l->password[0] = '\0';
			l->confirm[0] = '\0';
		}
		break;
	case MODEOPT_LAN_POINTS_TO_WIN: // 0..6, wrapping
		l->pointsIdx = (u8)wrapStep(l->pointsIdx, 0, 6, right);
		break;
	case MODEOPT_LAN_TIME_LIMIT:    // 0..4, wrapping
		l->timeIdx = (u8)wrapStep(l->timeIdx, 0, 4, right);
		break;
	case MODEOPT_LAN_DIRECTION:     // 0x4B4150's class: toggles
		l->reverse = l->reverse ? 0 : 1;
		break;
	case MODEOPT_LAN_LAPS:          // 0x4B3F10: the mode's min..10
		lanlobby_lap_range(l->mode, l->pointToPoint, &lo, &hi);
		l->laps = (u8)wrapStep(l->laps, lo, hi, right);
		break;
	case MODEOPT_LAN_MAX_PLAYERS:   // 0x4B3190, 2..6 here (PORT, see init)
		l->maxPlayers = (u8)wrapStep(l->maxPlayers, 2, LAN_CONSOLES, right);
		break;
	case MODEOPT_LAN_COLLISION:
		l->collision = l->collision ? 0 : 1;
		break;
	case MODEOPT_LAN_RESTRICTION:
		l->restriction = l->restriction ? 0 : 1;
		break;
	case MODEOPT_LAN_RACE_BREAKER:
		l->raceBreaker = l->raceBreaker ? 0 : 1;
		break;
	default:
		break;
	}
}

int modeopts_lan_accept_check(const ModeOptsLan* l) {
	if (!l->protect)
		return 0;
	const size_t n = strlen(l->password);
	if (n < 3 || n > 12)
		return 1;
	if (strcmp(l->password, l->confirm) != 0)
		return 2;
	return 0;
}
