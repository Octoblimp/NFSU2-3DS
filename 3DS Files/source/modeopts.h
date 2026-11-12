#pragma once
#include <3ds.h>
#include <stdbool.h>

// UI_QRModeOptions' option rows -- the per-race-type settings list behind the
// panel that TASKS.md #115 layer 2 left blank.
//
// Where the data lives
// --------------------
// None of this is authored in the .fng. UI_QRModeOptions.fng carries eleven
// anonymous OPTION_NAME_n / OPTION_DATA_n / OPTION_BACKING_n / LEFT_ARROW_n /
// RIGHT_ARROW_n slots and nothing else; which slots are used, what they say and
// what they do is all code in SPEED2.EXE. The chain, recovered end to end:
//
//   "UI_QRModeOptions.fng" string      .rdata 0x78F380
//   screen registry entry              .data  0x7F858C (stride 28)
//     -> factory 0x4FA550 -> new(0x164) -> ctor 0x4EF3F0
//        -> base ctor 0x552780 (installs the OPTION_* widget base names)
//        -> vtable 0x798014, slot 4 = 0x4E2C70 "populate rows"
//   0x4E2C70                           per-race-type dispatcher; Quick Race and
//                                      Split-Screen take the race type from the
//                                      settings object 0x83AAB4, LAN/Online from
//                                      0x850088, and jump through table 0x4E2D68
//                                      (0x4E2D94 for LAN/Online).
//
// The eight Quick-Race builders reached through 0x4E2D68 are, in race-type
// order: 0 Circuit 0x4CCF60, 1 Sprint 0x4CD2D0, 2 Drag 0x4CD530, 3 Drift
// 0x4CD7A0, 4 URL 0x4B47B0, 5 0x4B47B0, 6 (none), 7 0x4CDB40, 8 Outrun
// 0x4CDA20, 9 Free Run 0x4B4A10, 0xA Street X 0x4B47B0. Each one appends rows
// in a fixed order, and each row is a small C++ object whose vtable sits in the
// run 0x791220 + n*0x48 -- slot 1 is its LEFT/RIGHT handler and slot 3 is its
// refresh, which is what writes the OPTION_NAME_n / OPTION_DATA_n text. Those
// refreshes are where every label and value key below comes from; they were
// each resolved against the shipped LANGUAGES/English.bin.
//
// After its own rows every builder falls into the shared tail 0x4B41C0, which
// appends the three password rows plus (LAN only) points/time. Those rows hide
// themselves in refresh whenever 0x8500C8 is 0, which it always is outside a
// hosted online game, so Quick Race never shows them and this port does not
// build them.
//
// Nothing here is invented: the option set, its order, its gates, its strings
// and its wrap limits are transcriptions of the functions named above.

// The settings object
// -------------------
// The engine keeps one instance at 0x83AAB4 (uninitialised .data, i.e. BSS --
// there are no static bytes for it in the exe). Its accessors are the tight
// __thiscall stubs at 0x5026F0..0x502A40, and they all decode the same layout:
// a race type at +0x00 and a 20-byte block per race type at +0x10 + 20*type.
//
//   block +0x00 byte  min opponents  0x5026F0
//   block +0x01 byte  opponents      0x502710 get / 0x502960 set
//   block +0x02 byte  difficulty     0x502730 get / 0x502990 set
//   block +0x04 byte  traffic level  0x502750 get / 0x5029C0 set
//   block +0x05 byte  catch up       0x5028F0 get / 0x5029F0 set
//   block +0x08       race record    0x502910 returns a pointer to it
//
// Race type 0xC (the online party playlist) is the one exception: its fields
// are scattered across +0x100..+0x11C instead, and it owns the object's array
// of queued races. This port never reaches it, so RaceSettings keeps the
// regular 13th block and the accessors below take the non-0xC arm only.
typedef struct {
	u16 trackId;   // record +0x00
	u16 laps;      // record +0x02
	u32 direction; // record +0x04, 0 = forward, 1 = reverse
	u8 lapKO;      // record +0x08
	u8 pad[3];
} RaceRecord;

typedef struct {
	u8 minOpponents; // +0x00
	u8 opponents;    // +0x01
	u8 difficulty;   // +0x02
	u8 unk03;        // +0x03  written 1 by 0x515800, never read by any accessor
	u8 trafficLevel; // +0x04
	u8 catchUp;      // +0x05
	u8 unk06;        // +0x06  written 0 by 0x515800, likewise unread
	u8 pad07;
	RaceRecord record; // +0x08
} RaceModeBlock;

#define RACESET_MODES 13

typedef struct {
	int raceType;                   // +0x00
	RaceModeBlock mode[RACESET_MODES]; // +0x10, stride 20
	int pointsToWinIdx;             // +0x108, index into 0x7992CC
	int timeLimitIdx;               // +0x10C, index into 0x7992D4
} RaceSettings;

// SPEED2.EXE:0x515800 -- the object's initialiser, and the only place the
// shipped defaults exist (0x83AAB4 is BSS, so they are not bytes in the file).
void raceset_init(RaceSettings* s);

// The accessors, by their exe addresses. Each takes the non-0xC arm.
u8 raceset_min_opponents(const RaceSettings* s);  // 0x5026F0
u8 raceset_opponents(const RaceSettings* s);      // 0x502710
u8 raceset_difficulty(const RaceSettings* s);     // 0x502730
u8 raceset_traffic(const RaceSettings* s);        // 0x502750
u8 raceset_catch_up(const RaceSettings* s);       // 0x5028F0
RaceRecord* raceset_record(RaceSettings* s);      // 0x502910
const RaceRecord* raceset_record_const(const RaceSettings* s);

// Frontend context (global 0x850078) -- same values as TRACKDB_CTX_*. Repeated
// here because the builders and the opponents handler branch on it directly.
#define MODEOPTS_CTX_QUICKRACE 0x2
#define MODEOPTS_CTX_SPLITSCREEN 0x4
#define MODEOPTS_CTX_LAN 0x200 // TASKS #321b: the LAN create-game rows

// One option row, in the order the builder appended it. `kind` identifies the
// row class the exe instantiated; the vtable address is in the comment on each
// enumerator in modeopts.c.
typedef enum {
	MODEOPT_TRACK_DIRECTION = 0,
	MODEOPT_LAP_KO,
	MODEOPT_LAPS,
	MODEOPT_TRAFFIC,
	MODEOPT_OPPONENTS,
	MODEOPT_DIFFICULTY,
	MODEOPT_CATCH_UP,
	MODEOPT_POINTS_TO_WIN,
	MODEOPT_TIME_LIMIT,
	// The LAN arm (context 0x200, TASKS #321b): every LAN mode's builder
	// falls into FUN_004b41c0's list, read from the session block at
	// 0x850080 (ModeOptsLan below) rather than the Quick Race object.
	MODEOPT_LAN_PROTECTED,      // vtable 0x791220 "Password protected?"
	MODEOPT_LAN_PASSWORD,       // 0x791268 "Enter password" (text entry)
	MODEOPT_LAN_CONFIRM,        // 0x7912B0 "Confirm password" (text entry)
	MODEOPT_LAN_POINTS_TO_WIN,  // 0x7912F0, Outrun only
	MODEOPT_LAN_TIME_LIMIT,     // 0x791338, Outrun only
	MODEOPT_LAN_DIRECTION,      // 0x4B40B0
	MODEOPT_LAN_LAPS,           // 0x4B3E40
	MODEOPT_LAN_MAX_PLAYERS,    // 0x4B3110
	MODEOPT_LAN_COLLISION,      // 0x791100
	MODEOPT_LAN_RESTRICTION,    // 0x791148
	MODEOPT_LAN_RACE_BREAKER,   // 0x791190
} ModeOptKind;

#define MODEOPTS_MAX_ROWS 12

typedef struct {
	ModeOptKind kind;
	u32 labelHash; // English.bin key the row's refresh puts in OPTION_NAME_n
} ModeOptRow;

// A row's current value, as its refresh would format it into OPTION_DATA_n.
// Every row class produces one of three shapes: a bare language string, a bare
// number, or a number followed by a language string ("10 Minutes").
typedef struct {
	bool isNumber;
	int number;
	u32 valueHash;  // the string, when !isNumber
	u32 suffixHash; // the trailing string, when isNumber and non-zero
	char text[16];  // literal text, when non-empty (the password's stars)
} ModeOptValue;

// What the mode-options screen needs to know about the selected track. These
// are the two record fields the builders gate rows on -- see TrackRec.
typedef struct {
	u16 trackId;
	bool allowsReverse; // record +0x86 == 1
	bool allowsTraffic; // record +0x80 == 1
	// 0x5156F0(1, trackId, 1, 0): the career has unlocked the track
	// (eventdb_track_unlocked). Every builder but the circuit's adds Track
	// Direction only then.
	bool unlocked;
} ModeOptTrack;

// SPEED2.EXE:0x4E2C70 and the builder it dispatches to. Appends the rows for
// `s->raceType` in the engine's own order and returns how many were written.
// `ctx` is the frontend context global 0x850078. `s` is not const because the
// circuit builder clears a stale Lap KO flag before it lists anything.
int modeopts_build_rows(RaceSettings* s, int ctx, const ModeOptTrack* trk,
	ModeOptRow* rows, int maxRows);

// The row refreshes' value half (vtable slot 3 of each row class).
void modeopts_row_value(const RaceSettings* s, ModeOptKind kind, ModeOptValue* out);

// True when the row's own refresh disables itself (SPEED2.EXE:0x4B3DD0's
// tail): the Laps row, while Lap KO is on. A disabled row still occupies its
// slot -- the engine plays the widgets' "Disable" script rather than removing
// the row -- but it no longer takes LEFT/RIGHT.
bool modeopts_row_disabled(const RaceSettings* s, ModeOptKind kind);

// The row change handlers (vtable slot 1). `right` selects the RIGHT message
// (0xB5971BF1) over LEFT (0x9120409E); rows that ignore the distinction toggle
// either way, exactly as the exe does.
void modeopts_adjust(RaceSettings* s, int ctx, ModeOptKind kind, bool right);

// Every English.bin key a row value can resolve to, so a caller can pre-resolve
// them all while the language table is still open and then format a value
// without one. The list is exactly the VAL_* set the nine refreshes in
// modeopts.c use -- nothing extra, nothing invented.
#define MODEOPTS_VALUE_HASH_COUNT 19
const u32* modeopts_value_hashes(void);

// Index into modeopts_value_hashes() for `hash`, or -1. Used to look a value's
// pre-resolved string back up.
int modeopts_value_index(u32 hash);

// ---------------------------------------------------------------------------
// The LAN create-game rows (TASKS #321b, docs/formats/lan-lobby-pc-re.md s6)
//
// FUN_004e2c70 reads a LAN game's race type from 0x850088, and every LAN arm
// ends in the shared tail FUN_004b41c0, which in context 0x200 is the whole
// list: the three password rows, Outrun's points and time, Track Direction,
// Laps, Maximum Players, Collision, Car Performance Restrictions and Race
// Breaker. Ranked (0x791070) and Private Room (0x7911D8) are online's only.
// The values live in the session block at 0x850080; ModeOptsLan is its LAN
// half, the one main.c's create flow fills and hands to lanlobby_create().
typedef struct {
	u8 mode;            // 0x850088
	u16 track;          // 0x8500B8
	bool pointToPoint;  // the track's +0x80 == 1
	bool allowsReverse; // the track's +0x86 == 1 (and unlocked)
	u8 protect;         // 0x85009B, reset to 0 on entry (FUN_004b41c0)
	char password[13];  // 0x85009C, 3..12 typed
	char confirm[13];   // 0x8500A9
	u8 pointsIdx;       // 0x850090, into 0x7992CC
	u8 timeIdx;         // 0x850094, into 0x7992D4
	u8 reverse;         // 0x8500BC
	u8 laps;            // 0x8500BA
	u8 maxPlayers;      // 0x850080
	u8 collision;       // 0x85009A
	u8 restriction;     // 0x85008C: 1 Evenly Matched, 0 None
	u8 raceBreaker;     // 0x850099
} ModeOptsLan;

// The block as the create flow starts it for `mode` on `track`: the rows'
// own forcings (no direction for Outrun or a track without one, one lap
// where there is no Laps row, no Race Breaker for Drift / Street X).
void modeopts_lan_init(ModeOptsLan* l, u8 mode, u16 track, bool pointToPoint,
	bool allowsReverse);
// FUN_004b41c0 in context 0x200: the rows in its order.
int modeopts_lan_build_rows(ModeOptsLan* l, ModeOptRow* rows, int maxRows);
void modeopts_lan_row_value(const ModeOptsLan* l, ModeOptKind kind, ModeOptValue* out);
// FUN_004982c0: Enter password only while protected; Confirm only while
// protected with a password typed.
bool modeopts_lan_row_disabled(const ModeOptsLan* l, ModeOptKind kind);
void modeopts_lan_adjust(ModeOptsLan* l, ModeOptKind kind, bool right);
// A text-entry row (the two password rows): A opens the keyboard.
bool modeopts_lan_row_is_text(ModeOptKind kind);
// Accept's checks (0x4EF620): 0 ok, 1 bad length (0xD29A1A18 with 3 and 12),
// 2 the two passwords differ (0xFF99D845).
int modeopts_lan_accept_check(const ModeOptsLan* l);
