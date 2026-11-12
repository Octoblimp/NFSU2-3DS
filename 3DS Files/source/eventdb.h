#pragma once
#include <3ds.h>
#include <stddef.h>

// Runtime parser for the career event table, and the unlock query the track
// selection ring is gated on.
//
// The table lives in GLOBAL/GLOBALB.BUN as sub-chunk 0x34A11 of a top-level
// 0x80034A10 container. SPEED2.EXE:0x512C00 is the loader: it is handed each
// container, bails unless the magic matches, then walks the container's
// sub-chunk list and parks each payload in a global. For 0x34A11 that is
// `DAT_008383E0 = payload; DAT_008383E4 = size / 0x88` -- the record stride is
// 0x88 and the count is derived, not stored.
//
// 0x512C00 opens with `if (DAT_008641F4 != 0) return 1; DAT_008641F4 = 1;`, a
// one-shot guard, so only the *first* container in the file is ever decoded.
// That matters here: the shipped GLOBALB.BUN carries two 0x80034A10 containers,
// the full one at chunk #208 (224 records) and a stripped one at #210 (8
// records). The guard means the 224-record table is the live one and the
// stripped one is dead weight, so eventdb_load() stops at the first container
// too rather than letting the last writer win.

// The shop table is the sibling sub-chunk, 0x34A12 of the same 0x80034A10
// container, and the same one-shot guard applies to it -- 0x512C00 parks every
// sub-chunk it recognises in one pass, so the first container's copy is the
// live one here too. Stride 0xA0, which is the stride
// FindShopDataDescByTriggerZone (Speed.elf 0x80121F54) walks the array by.
//
// Nothing below is guessed. The shipped PC GLOBALB.BUN carries 24 records, and
// the 23 SHOP_TRIGGER polygons in TRACKS/ROUTESL4RA/PathsFreeRoam.bin
// (trackpath.h) hold 23 distinct handles -- all 23 match a record's +0x3C,
// which is what fixes that field as the trigger handle. The 24th record is
// THE_UNIQUES_SHOP, which has no free-roam trigger of its own.
//
// Each record's own name then confirms the kind byte at +0x50 independently:
// UC_BODYSHOP and SH_BODYSHOP are kind 2, UC_CAR_LOT is kind 4, UC_PERF_SHOP
// is kind 3, CC_GRAPHICS is kind 1, UC_CAR_SPECIALTY and CC_CAR_SPECIALTY are
// kind 5 -- and engage.c's shop-icon table, recovered separately from
// _get_shop_indicator_type (0x80111CA4), pairs those same kinds with
// SHOP_ICON_BODY, SHOP_ICON_CAR_LOT, SHOP_ICON_PERFORMANCE, SHOP_ICON_GRAPHIC
// and SHOP_ICON_INTERIORS. Two independent sources, one mapping.
#define EVENTDB_MAX_SHOPS 64

#define EVENTDB_MAX_EVENTS 256

// An event record's track list is four entries of four bytes at +0x18..+0x27.
// That bound is read out of the engine, not inferred from the record size:
// EndianSwap__15CareerEventData (Speed.elf 0x8012185C) walks the list with
// `mtctr 4` over a pointer it advances by 4, and the very next field it swaps
// is the u32 at +0x28 -- so a fifth entry would be the trigger handle, not a
// track. The shipped table uses at most three (205 records carry one, 11 carry
// two, 8 carry three), so the old bound of seven never actually over-read; it
// was still wrong about where the list ends.
#define EVENTDB_MAX_EVENT_TRACKS 4

// Event record +0x18 + i*4. The flag byte is the one 0x514340 tests.
typedef struct {
	s16 trackId;
	u8 flags;
	u8 extra;
} EventTrackEntry;

// RaceStarter's own loop bound, the event record's +0x7C: it never exceeds 5
// in the shipped table (12 records carry 0, one 1, three 2, 184 three and 24
// five), and the opponent array at +0x40 has room for exactly five 0x0C-byte
// entries before +0x7C.
#define EVENTDB_MAX_OPPONENTS 5

// bStringHash("RANDOM"), the value AddCareerEventAIOpponents compares `car`
// against before picking a car instead of loading a preset. The string is at
// Speed.elf 0x80327FA4; the hash is recomputed here rather than hard-wired by
// eye -- 672 of the 679 shipped opponents carry it.
#define EVENTDB_OPPONENT_CAR_RANDOM 0x7710F0E0u

// One CareerEventOpponentData, the 0x0C-byte record at event +0x40 + i*0x0C.
//
// The layout is AddCareerEventAIOpponents' own (Speed.elf 0x8011A22C), which
// reads every field of it, and each one was then checked against all 679
// opponents in the shipped PC GLOBALB.BUN:
//
//   +0x00 u16  name, a career string blob offset, exactly like the event's own
//              (GetName__23CareerEventOpponentData 0x80121AA0 is an lhz and a
//              GetCareerStringAtOffset). 126 distinct first names.
//   +0x02 s8   how this opponent's performance is matched -- see EventOppMatch.
//              The shipped table uses only 1 (616 entries) and 3 (63).
//   +0x03 s8   the performance measure when the mode is MATCH_FIXED, taken as
//              a float. Always 0, because no shipped record uses that mode.
//   +0x04 u32  the car: bStringHash of a preset name, or RANDOM above. 672
//              RANDOM, 7 named.
//   +0x08 s8[3] the driver's skill, one per difficulty. The engine indexes it
//              with the difficulty global (DAT_803FCAB8) and hands the byte to
//              DriverInfo's constructor. Every shipped triple ascends.
//   +0x0B s8   DriverInfo's last argument. Always 1.
typedef struct {
	char name[20];
	s8   match;      // +0x02, an EventOppMatch
	s8   matchValue; // +0x03
	u32  carHash;    // +0x04
	s8   skill[3];   // +0x08, by difficulty
	s8   flag;       // +0x0B
} EventOpponent;

// The +0x02 modes, named for what the engine does with each.
typedef enum {
	// RidePhysicsInfo::MatchPerformance against the player's own ride: the
	// opponent is built to the player's exact performance measure.
	EVENTOPP_MATCH_PLAYER = 1,
	// No matching at all -- whatever the preset or the random pick came with.
	EVENTOPP_MATCH_NONE = 2,
	// RuntimeCareerStageInfo::GetAIPerformanceMatchingParameter for the
	// career's current stage, plus the engine's own bias.
	EVENTOPP_MATCH_STAGE = 3,
	// The measure is the record's own +0x03 byte. Unused by the shipped table.
	EVENTOPP_MATCH_FIXED = 0,
} EventOppMatch;

// The event's race type, the signed byte at +0x0F. The names are retail's own:
// GetEngageDialogBody (Speed.elf 0x80121B88) selects a dialog family per type
// and those families are spelled ENGAGE_CIRCUIT_RANDOM_%d, _SPRINT_, _STREET_,
// _DRAG_ and _DRIFT_. Every one of the 224 shipped records then agrees with its
// own name independently -- 185 names carry their type (STAGE_1_DRIFT_1 is 5,
// STAGE_1_SHORT_1 is 2, S4_SPON_DRAG_7 is 4) and not one disagrees.
//
// Type 3 is reached only by DDAY_EVENT_A and DDAY_EVENT_B. It has no dialog
// family: the `else` arm of GetEngageDialogBody sets a variant count for it but
// leaves the format-string register at the 0 it was initialised with, so the
// call would hash a NULL. That is retail's own latent bug and it is unreachable
// in practice, because both DDAY records carry class 0, which takes an earlier
// arm. Transcribed as found rather than repaired.
typedef enum {
	EVENT_TYPE_CIRCUIT = 0,
	EVENT_TYPE_SPRINT  = 1,
	EVENT_TYPE_STREET  = 2, // Street X -- retail's own string is _STREET_
	EVENT_TYPE_DDAY    = 3,
	EVENT_TYPE_DRAG    = 4,
	EVENT_TYPE_DRIFT   = 5,
	EVENT_TYPE_COUNT
} EventType;

// The event's class, the byte at +0x34. Three functions agree on it:
// GetEngageDialogBody picks the ENGAGE_SPON_* family for 1 and ENGAGE_URL_* for
// 2, _get_career_event_indicator_type (0x80111C1C) returns the URL icon for 2,
// and GetEventPurse (0x801275B0) takes the purse from the career stage instead
// of the record for 1. The shipped names agree: every S*_SPON_* record is 1,
// every S*_URL_* record is 2, and class 0 is held by exactly the two DDAY
// records.
typedef enum {
	EVENT_CLASS_DDAY    = 0,
	EVENT_CLASS_SPON    = 1,
	EVENT_CLASS_URL     = 2,
	EVENT_CLASS_REGULAR = 3
} EventClass;

// One 0x88-byte event record, reduced to the fields this port reads. The rest
// is race setup -- opponent rosters, rewards, per-mode tuning -- which belongs
// to layers that are not wired yet.
//
// Every offset below is taken from EndianSwap__15CareerEventData, which
// enumerates the record by swapping each field at its own width, and then
// checked against the shipped table before being used. The checks that fix
// them are recorded next to each field.
typedef struct {
	// +0x00, a u16 offset into the career string blob (sub-chunk 0x34A1D), not
	// an inline string: GetEventName__15CareerEventData (0x80121AC4) reads the
	// u16 and adds it to the blob base. All 896 string offsets in the shipped
	// table (four per record) land inside the blob.
	char name[32];

	// +0x08. GetStateRecord/HasCompletedEvent search the save's progress table
	// by this value, and it is the uppercase-folded bStringHash of `name` --
	// confirmed on all 224 shipped records, with savefile.md's independently
	// recorded DDAY_EVENT_A = 0xDD60E402 agreeing.
	u32 eventId;

	// +0x10, read two ways. BuildListOfEvents (Speed.elf 0x80125A30) reads it
	// as an event id on one record, S5_CIRCUIT_36 (0x04CCE3E1), whose +0x10 is
	// S5_URL_9: on stage 5, once that is won and S5_CIRCUIT_36 is not, the
	// pile is S5_CIRCUIT_36 alone. CanDoURL (0x80124DAC) reads it on a URL
	// event as the counts urlRule (+0x0C) asks for -- see there. The other
	// records carry the stage before's number or an event id (the stage 1
	// world races name DDAY_EVENT_B), which nothing decoded reads.
	u32 field10;

	// +0x0C, CanDoURL's rule for a URL event (class 2) to join the pile:
	//   3  (s32)field10 >> 16 <= the stage's world races won
	//      (S4_URL_1's 0x00010004 asks for 1)
	//   4  the stage's required showcases done, and field10's bytes
	//      (>> 8 & 0xFF) <= world races, (>> 24) <= URL races and
	//      (>> 16 & 0xFF) <= sponsor races won (S5_URL_9's 0x08032305:
	//      35 world, 8 URL, 3 sponsor)
	//   anything else: never. 0..2 on the non-URL records, unread there.
	s8 urlRule;

	// +0x28, the free-roam TrackPathZone handle that offers this event. 222 of
	// the 224 records name a real RACE_START_TRIGGER polygon; the two that do
	// not are the DDAY pair, which have no free-roam trigger. Note the mapping
	// is many-to-one: 65 start points serve more than one event, so a handle
	// alone does not identify an event: which one is open is the career's
	// EventPile's answer (GetFirstUnlockedEvent, main.c's careerFirstUnlockedEvent).
	u32 triggerHandle;

	u32 purse;     // +0x30, what CommitWinEvent deposits (Speed.elf 0x80127100)
	s8  type;      // +0x0F, an EventType
	s8  cls;       // +0x34, an EventClass
	s8  traffic;   // +0x36, the traffic level 0..3 StartCareerEvent (0x8011D02C)
	               // writes to TheRaceParameters +0x08 (AddTrafficCars reads it)
	s8  stage;     // +0x37, the career stage; indexes RuntimeCareerStageInfo
	               // in CommitWinEvent, and agrees with the S<n>_ prefix on all
	               // 214 shipped records that carry one
	u8  isSUV;     // +0x0D; gates on DoesPlayerOwnSUV. Set on the S*_SUV_* names
	u8  isHidden;  // +0x7F; 33 records, 32 of them named *_H_*. IsEventVisible-
	               // OnRadar returns false for these, and NotifyEventDiscovered
	               // gives them a permanent minimap marker once driven into
	EventTrackEntry tracks[EVENTDB_MAX_EVENT_TRACKS];
	s8 trackCount; // +0x7E, a signed byte in the engine's own loops

	// +0x7C, read with lbz+extsb by AddCareerEventAIOpponents (Speed.elf
	// 0x8011A22C) as the loop bound over the opponent array at +0x40, and
	// stored straight into TheRaceParameters+0x28 by StartCareerEvent. The
	// shipped table carries only 0..5 of them -- 12 zeroes, one 1, three 2s,
	// 184 threes and 24 fives -- i.e. the field counts cars, not bytes.
	u8 opponentCount;

	// +0x40, the roster RaceStarter puts on the grid behind the player. Only
	// the first `opponentCount` entries are filled.
	EventOpponent opponents[EVENTDB_MAX_OPPONENTS];

	// Opponent 0's name, resolved out of the career string blob at load the
	// same way the event's own name is. GetEngageDialogFrom (0x80121D54) uses
	// it as the engage dialog's sender whenever the record has no sender string
	// of its own, which is 223 of the 224. The same string as opponents[0].name.
	char fromName[24];

	// +0x84, a localized string hash, NOT a zero field. GetEngageDialogFrom
	// prefers it over the opponent's name when it is nonzero, which happens on
	// exactly one shipped record: S2_URL_1, whose 0x770AF86E resolves in
	// English.bin to "Rachel".
	u32 senderStringId;

	// +0x3C, the shop a shop event sends the player to: a ShopRec's `handle`.
	// PlayerCareerState::MaybeTargetGPSForShopEvent (Speed.elf 0x80126BB4) follows it
	// to the shop's trigger zone and points the GPS there -- the car lot for
	// DDAY_EVENT_A.
	u32 shopHandle;

	// +0x2C, the preset car BuildRideForPlayer (Speed.elf 0x8012731C) hands the
	// player as the event is entered: bStringHash of a PresetRide name, or 0
	// to keep the current ride. Only DDAY_EVENT_A names one in the shipped
	// table, 0xF7E19102 = DDAY_PLAYER_CAR (tools/re/wip268/presetcars.py).
	u32 presetCar;

	// +0x02 / +0x04, u16 career blob offsets like the name's: the movies
	// IG_PlayMovie plays before the race (AcceptRace, Speed.elf 0x800E81E4)
	// and after it (CalcShowPostRaceMovie, 0x801267F0; SPEED2.EXE 0x513A50).
	// Empty on most events; e.g. STAGE_1_DRAG_1's pre is "DRAGTUT", S2_URL_3's
	// post "SUBURBAN" (tools/re/wip268/movies.py lists them all).
	char preMovie[16];
	char postMovie[16];
} EventRec;

// One 0xA0-byte ShopDataDesc record, reduced to the fields the free-roam HUD
// and a shop walk-in read. The rest is the shop's inventory, which belongs to
// shop screens this port has not reached.
typedef struct {
	char name[32]; // +0x00, e.g. "UC_BODYSHOP" -- kept because it is the one
	               // field that makes a wrong mapping obvious in the log
	u32 handle;        // +0x38, what an event's +0x3C names to send the
	                   // player here (FindShopDataDescByHandle)
	u32 triggerHandle; // +0x3C, the TrackPathZone handle that opens this shop
	// +0x40, the room the shop's frontend stands in: a file under
	// FRONTEND\PLATFORMS, e.g. "PERFSHOP01.BIN", which
	// GameFlowLoadingFrontEndPart1 loads for a shop walk-in. Sixteen bytes up
	// to +0x50; the longest shipped name, "PARTSSHOP01.BIN", fills them with
	// its terminator.
	char platform[16];
	s8 kind;           // +0x50, the shop type; see engage.c's icon table

	// What PlayerCareerState::IsShopEnterable (Speed.elf 0x801243B8) and
	// ShopDataDesc::IsShopUnlocked (0x80122044) read, which is what the
	// performance parts the unlock table gates on a shop are gated on
	// (perfdb.h, PERFDB_UNLOCK_SHOP).
	//   needsEvent  +0x51: when set, the shop opens only once the career state
	//               record keyed on this shop's own `handle` is completed.
	//   unlockArg   +0x74: the event id or race count IsShopUnlocked's kind
	//               takes.
	//   unlockKind  +0x9C: 0 always, 1 HasCompletedEvent(unlockArg), 2 URL
	//               races, 3 world races, 4 sponsor races -- each of the last
	//               three `>= unlockArg` in stage `stage`. Anything else is
	//               locked.
	//   stage       +0x9D: the career stage the shop opens in, and the stage
	//               IsShopUnlocked counts races in.
	s8 needsEvent;
	u32 unlockArg;
	s8 unlockKind;
	s8 stage;

	// +0x20, inline up to +0x38: the movie MaybeEnterShop (Speed.elf
	// 0x800E7B20) plays on the first walk-in, before LaunchEngagedShop.
	// "SCENE05" on CC_CAR_LOT_1, "SCENE06" on CRIB_1, empty elsewhere.
	char movie[24];
} ShopRec;

// The career's per-stage data, sub-chunk 0x34A18 of the same container.
// LoaderMostWantedCareerChunk (Speed.elf 0x80123520) hands it to
// CreateStageDataList (0x80122efc), whose count is `size / 0x50` -- the same
// derived-count shape the event and shop tables have -- and whose byte-swap
// pass enumerates the record the way CareerEventData's does. The shipped table
// is six records, one per stage, and each one's +0x00 is its own stage number
// 0..5, which is the key FindStageDataByStage (0x801230a0) matches on.
#define EVENTDB_MAX_STAGES 8

// The sponsors a stage offers. CreateStageDataList's swap pass resolves the
// five +0x08 words from name hashes to SponsorData pointers
// (FindSponsorDataByName); here they stay hashes and eventdb_sponsor_by_hash()
// is the lookup.
#define EVENTDB_STAGE_SPONSORS 5

typedef struct {
	s8  stage;            // +0x00, the stage this record is for
	s8  sponsorCount;     // +0x01, how many of sponsorHash[] are live -- the
	                      // bound GetChooseableSponsorByIndex (0x80124ACC) and
	                      // GetNumSponsorsToChooseFrom (0x801249F8) loop to. 0
	                      // on stages 0 and 1, 5 on the rest
	u16 encounterPurse;   // +0x02, GetRandomEncounterPurse (0x801275f4). 100
	                      // on stages 0..3, 250 on stage 4, 400 on stage 5
	u16 starRatingPurse;  // +0x04, GetStarRatingPurse (0x80127628). 0 on
	                      // stages 0 and 1, 100 on the rest
	u32 sponsorHash[EVENTDB_STAGE_SPONSORS]; // +0x08, bStringHash of each name
	// +0x1C: the world races the stage needs won before sponsor i is offered
	// (NeedToAskForSponsor, 0x80124940, compares it with
	// GetNumWorldRacesCompleted). 1 for every shipped sponsor.
	s16 reqWorldRaces[EVENTDB_STAGE_SPONSORS];
	// +0x28: the event whose win clears the stage, or 0 for "every event of
	// the stage". CheckIfClearedStage (0x80124CE8) tests it first: non-zero
	// asks HasCompletedEvent(clearEvent) (and the stage's showcases); zero
	// falls to AllEventsInStageCompleted. Stage 0's is DDAY_EVENT_B
	// (0xDD60E403), the crib visit; stage 1 is the one shipped record with 0.
	u32 clearEvent;
	// +0x42 / +0x44 / +0x48: GetAIPerformanceMatchingParameter (0x80124598),
	// the 0..10 measure a STAGE opponent is upgraded to -- aiPerfLo rising to
	// aiPerfHi as the stage's world, sponsor and URL races get won, aiPerfHi
	// once more than aiRaces are (or when aiRaces is 0). Shipped: stage 0
	// {1, 0, 0}, 1 {5, 0, 1}, 2 {16, 0, 3}, 3 {28, 3, 4}, 4 {40, 4, 8},
	// 5 {47, 8, 10}.
	s16 aiRaces;
	float aiPerfLo;
	float aiPerfHi;
	// +0x30..+0x37: how many events of each kind the stage's EventPile holds
	// at once. GetMaxNumberOfEventsByType (0x80125614) reads them as signed
	// bytes: circuit +0x30, drag +0x31, Street X +0x32, drift +0x33, sprint
	// +0x34 and SUV +0x37 (+0x35/+0x36 are read by nothing decoded). Shipped:
	// 3 of each on stage 0, 1 on stage 1, 2 on stage 2, 3 on stage 3 and 4 on
	// stages 4 and 5, with stage 5's SUV at 4.
	s8 maxByKind[8];
	// +0x41, the hidden events (*_H_*) the pile takes at most; -1 (255 in the
	// data, read signed) for no cap. Shipped: 255, 255, 4, 8, 10, 10.
	s8 hiddenCap;
	// +0x40: the outruns the stage allows won, -1 (255) for no cap.
	// RandomEncounterState::CheckForRightConditions (SpeedPS.elf 0x80127890)
	// offers no outrun once the stage's RuntimeCareerStageInfo +0xDC reaches it.
	s8 outrunCap;
} StageRec;

// GetAIPerformanceMatchingParameter for stage record `s` with `racesWon` of
// its world, sponsor and URL races won.
float eventdb_stage_ai_perf(const StageRec* s, int racesWon);

// One car-unlock record, sub-chunk 0x34A1F of the same container, 12 bytes
// each: {u32 car name hash (CarTypeInfo+0xD0), u32 event id per build region}.
// BuildUnlockedCareerCarList (Speed.elf 0x801273D8) starts with every car type
// unlocked and locks a record's car while the event for the current region
// (+0x04 on region 0, +0x08 otherwise) exists and is not yet won -- an id that
// names no event leaves the car unlocked, which is MIATA's 0. The shipped table
// is 24 records.
#define EVENTDB_MAX_CAR_UNLOCKS 32

typedef struct {
	u32 carHash;
	u32 eventId[2];
} CarUnlockRec;

// One CareerBankTrigger, sub-chunk 0x34A1E of the same container, 12 bytes
// each (#293). LoaderMostWantedCareerChunk (Speed.elf 0x80123520) hands it to
// LoadBankTriggers (0x80123318), which byte-swaps it in place: {u16 amount,
// u8 type, u8 flag index, u32 param, u32 zone hash}. FindCareerBankTrigger
// (0x801233BC) matches the zone hash against a BANK_TRIGGER zone's +0x30.
// CanClaimBankReward (0x80126164): type 1 needs the career stage
// (PlayerCareerState +0x71E4) above `param`, type 2 HasCompletedEvent(param),
// anything else nothing; then the claimed flag at +0x7E58 + index must be 0.
// The PC's GLOBALB.BUN ships 59: two type 0 and 57 type 1, params 0..4.
#define EVENTDB_MAX_BANK_TRIGGERS 128

typedef struct {
	u16 amount;
	u8  type;
	u8  flag;
	u32 param;
	u32 zoneHash;
} BankTriggerRec;

// One ShowcaseData, sub-chunk 0x34A16 of the same container, 0x40 bytes each
// (GetShowcaseDataByIndex, Speed.elf 0x80122580: base [r13-0x7174] + i << 6,
// count [r13-0x7178]). CreateShowcaseAreaList (0x80122388) byte-swaps +0x20,
// +0x26 (u16), +0x28, +0x2C, +0x30, +0x36 (u16), +0x38 and +0x3C, which fixes
// the widths; the readers named below fix the meanings. The shipped table is
// 30 showcases; the ten required ones are the STAGE_<n>_DVD_<k> covers, one on
// stage 2 rising to four on stage 5, and stages 0 and 1 have none at all.
#define EVENTDB_MAX_SHOWCASES 40

typedef struct {
	char  name[32];     // +0x00, inline (STAGE_2_SHOW_1_PICTURE, STAGE_3_DVD_2)
	u32   handle;       // +0x20, bStringHash(name): the state record's id and
	                    // FindShowcaseDataByHandle's key (0x801224D4)
	s8    type;         // +0x24, 1 in-game magazine, 2 in-game DVD, 3 frontend
	                    // magazine, 4 frontend DVD (DoesThisRequireFE: 3 and 4)
	s8    stage;        // +0x25
	u16   field26;      // +0x26, 500 on all 30
	u32   magHash;      // +0x28, the magazine / cover the shoot is for:
	                    // AddWorldChallengeMessage's "_TITLE" continuation hash
	u32   triggerHandle;// +0x2C, the zone FindShowcaseDataByTriggerZone
	                    // (0x80122514) matches
	float snapshotTime; // +0x30, InitSnapshotTimeLimit's argument; 0 on all 30
	s8    cond;         // +0x34, what unlocks it: 0 any shop, 1 the shop at
	                    // +0x38, 2 the event at +0x38, 3 pictures, 4 a DVD star
	u8    required;     // +0x35, the stage needs it done before it can clear
	                    // (CheckThatRequiredShowcasesHaveBeenCompleted)
	u16   field36;      // +0x36, 0 on all 30
	u32   condArg;      // +0x38
	float starRating;   // +0x3C, RecalcDVDUnlocks' star threshold
} ShowcaseRec;

// One WorldChallengeData, sub-chunk 0x34A1B of the same container, 0x18 bytes
// each. CreateWorldChallengeDataList (Speed.elf 0x80122DB0) takes size/0x18 of
// them and byte-swaps three u16s (+0x00, +0x02, +0x04) and three words (+0x08,
// +0x0C, +0x10); the bytes from +0x06 and +0x14 on are left alone. The
// readers: GetChallengeName / GetStartAtTriggerName (0x80122D68 / 0x80122D8C,
// career blob offsets), FindWorldChallengeByTriggerZone (0x80122E1C, which
// compares bStringHash of the start trigger's name against the zone),
// MaybeAddWorldChallenge (0x80125104, +0x04 .. +0x07), AddWorldChallengeMessage
// (0x8012BD88, +0x08 and +0x14 / +0x15), WorldChallengeState::NotifyEnter
// (+0x0C, +0x10) and GetUniquesPartsList (+0x15 .. +0x17). The shipped table
// is 28 challenges, stages 2 to 5: twenty magazine shoots and eight uniques
// runs to a shop.
#define EVENTDB_MAX_CHALLENGES 32

// The unlock counters MaybeAddWorldChallenge switches +0x06 on.
#define WC_UNLOCK_WORLD_RACES 0  // GetNumWorldRacesCompleted
#define WC_UNLOCK_URL_RACES   1  // GetNumURLRacesCompleted
#define WC_UNLOCK_OUTRUNS     2  // RuntimeCareerStageInfo +0xDC, outrun wins
#define WC_UNLOCK_SPONSOR     3  // the sponsor races

// +0x14, the challenge's kind: what GetEngageChallengeDialogBody/Subject and
// AddWorldChallengeMessage switch on.
#define WC_TYPE_VISUAL   1
#define WC_TYPE_PERF     2
#define WC_TYPE_3        3
#define WC_TYPE_SHOWCASE 4

typedef struct {
	char name[32];         // +0x00, a career blob offset (STAGE_2_SHOW_1)
	u32  nameHash;         // bStringHash(name): the state record's id
	char startTrigger[40]; // +0x02, a career blob offset; the longest shipped
	                       // is TRIGGER_START_STAGE_3_VISUAL_2, 30 characters
	u32  startHandle;      // bStringHash(startTrigger): the start zone's handle
	s16  stage;            // +0x04
	s8   unlockKind;       // +0x06, WC_UNLOCK_*
	s8   unlockCount;      // +0x07, the counter must reach this
	u32  smsHash;          // +0x08, the SMS that announces it
	u32  goalHandle;       // +0x0C, a shop's +0x38 handle or a showcase's +0x20
	s32  timeLimit;        // +0x10, seconds
	u8   type;             // +0x14, WC_TYPE_*
	s8   sub[3];           // +0x15, the uniques part kinds; -1 / 0 = none
} WorldChallengeRec;

// One SponsorData, sub-chunk 0x34A19 of the same container, 16 bytes each.
// CreateSponsorDataList (Speed.elf 0x80122BE8) takes `size >> 4` of them and
// byte-swaps each with SponsorData::EndianSwap; the readers are
// uiSponsorsMain::RefreshHeader (0x800F9EB4), GetEventPurse (0x801275B0),
// NotifyChooseSponsor (0x80125E80) and GetNumSponsorsToChooseFrom. The shipped
// table is 20 sponsors, five a stage from stage 2, each stage's five in
// ascending minAvgPoints.
#define EVENTDB_MAX_SPONSORS 32

typedef struct {
	char name[24];      // +0x00, a u16 offset into the career string blob
	                    // (GetSponsorName); "NITROUS_EXPRESS" is the longest
	u32  nameHash;      // bStringHash(name): FindSponsorDataByName's key, and
	                    // what a save keeps in place of the pointer
	u16  purse;         // +0x02, a sponsored race's purse (GetEventPurse) and
	                    // the screen's "Sponsor Race Purse:"
	u8   raceType[4];   // +0x04, the race types the contract asks for, one
	                    // eSponsorCategories each (1 circuit, 2 drift, 3 drag,
	                    // 4 sprint, 5 Street X; 0 none -- the fourth byte is 0
	                    // on every shipped sponsor). GetMaxSponsoredEventTypes
	                    // (0x80125540) counts all four; SPONSOR_INFO_<name> is
	                    // the screen's text for them
	u32  field8;        // +0x08, not read by any function decoded so far
	s16  bonus;         // +0x0C, the signing bonus NotifyChooseSponsor pays
	s16  minAvgPoints;  // +0x0E, the average sponsor points per race the
	                    // stage before must have reached to be offered it
} SponsorRec;

// One SMSMessageData, sub-chunk 0x34A17 of the same container, 0x14 bytes
// each. CreateSMSMessageDataTable (Speed.elf 0x8012B6DC) takes size/0x14 of
// them and byte-swaps each with SMSMessageData::EndianSwap (0x8012B644) --
// which is what fixes the layout, because the swap enumerates exactly the
// fields wider than a byte: the u16 at +0x00, then a two-iteration loop over
// the words at +0x08 and +0x0C, then the word at +0x10. Everything between
// +0x02 and +0x07 it leaves alone, so those six are u8s.
//
// Every field below is read by a named function, and each was then checked
// against all 167 shipped records in the PC GLOBALB.BUN:
//
//   +0x00 u16  the message's name, a career string blob offset exactly like an
//              event's (GetMessageName 0x8012B6B8 is an lhz and a
//              GetCareerStringAtOffset). The names are SMS_INSTRUCTION,
//              SMS_UNLOCK_nn, SMS_MAG_*, SMS_DVD_COVER_n, ENGAGE_TIPS_* and so
//              on, and the name is the whole of the message: the subject and
//              the body are language labels built from it (see sms.h).
//   +0x02 s8   what raises it. The four IsTriggeredBy* predicates name five of
//              the values outright -- 1 completing an event
//              (IsTriggeredByCompletingEvent 0x8012B7FC, matching arg0/arg1
//              against the event's id), 2 a named shop (IsTriggeredByShop
//              0x8012B940, against ShopDataDesc+0x38), 7 entering a zone
//              (IsTriggeredByEnterZone 0x8012B98C), 8 any shop at all, and
//              0xE a DVD cover (IsTriggeredByHavingDVDCover 0x8012B91C). The
//              shipped table also uses 3..6, 9, 10, 12, 13 and 15, which no
//              decoded predicate reads, so they stay unnamed here.
//   +0x03 s8   the mailbox category: the value CanDisplayMessageInCurrentList
//              (0x800DD1DC) compares against the viewer's current tab. The
//              shipped table uses 2 (Game Tips), 3 (Special Events), 4
//              (Rachel) and 5 (Unlocks); 0 is Misc and 1 is the Inbox, which
//              no record carries because the Inbox is where everything not yet
//              sorted shows. GetMailboxCategoryLanguageHash (0x800DC84C) has
//              the label for each -- see SMS_CAT_* below.
//   +0x04 u8   auto-popup. AnyUnseenAutoPopups (0x8012BC38) tests this against
//              the queue slot's own "popped" byte, and MarkAllAutoPopupsSeen
//              (0x8012BCC4) sets that byte on every record carrying a 1.
//   +0x05 u8   which mailbox lists it can appear in, as a bitmask: 1 on every
//              record, plus 2 for category 3, 4 for category 5 and 8 for
//              category 2. Not read by any decoded function -- the viewer goes
//              through +0x03 -- and kept because it is real shipped data.
//   +0x06 u8   0x88 on all 167 records.
//   +0x07 u8   0 on all 167 records.
//   +0x08 u32  first argument, its meaning set by the trigger: an event id, a
//              shop handle, a zone handle, a DVD cover number, or for the
//              ENGAGE_TIPS_* family a language hash.
//   +0x0C u32  second argument. Both IsTriggeredByShop and
//              IsTriggeredByEnterZone walk +0x08 and +0x0C as a two-entry
//              list, which is why they are a pair rather than two unrelated
//              fields. 0 on every record but ENGAGE_TIPS_*, where it is 25 or
//              50.
//   +0x05 u8   an SMSMessageCategories, which is NOT the mailbox category at
//              +0x03: SMSGetEnvelopeImageHashAndColour (0x800DC308) switches
//              on it 1..7 for the colour SyncCurrentButton tints the row's
//              envelope with, and anything else takes the default green.
//   +0x05 u8   an SMSMessageCategories, which is NOT the mailbox category at
//              +0x03: SMSGetEnvelopeImageHashAndColour (0x800DC308) switches
//              on it 1..7 for the colour SyncCurrentButton tints the row's
//              envelope with, and anything else takes the default green.
//   +0x10 u32  the sender, a language hash GetFrom (0x8012BA04) returns
//              straight out. The shipped table uses eleven: Rachel (under two
//              different hashes), the four Bayview shops, and Chuck, Kristian,
//              Scott, Serena and Stefan.
// The longest shipped name is 18 characters. sms.c builds `<NAME>_SUBJECT`
// and `<NAME>_BODY` off the same bound, so it lives here rather than in
// eventdb.c.
#define EVENTDB_MAX_NAME 32
#define EVENTDB_MAX_SMS 192

typedef struct {
	char name[EVENTDB_MAX_NAME]; // +0x00, resolved through the career string blob
	u32  nameHash;   // FEngHashString(name): GetSMSMessageDataByNameHash's key,
	                 // and what a save slot holds in place of the pointer
	s8   trigger;    // +0x02
	s8   category;   // +0x03
	u8   autoPopup;  // +0x04
	u8   envelope;   // +0x05, an SMSMessageCategories
	u8   field6;     // +0x06, 0x88 throughout
	u8   field7;     // +0x07, 0 throughout
	u32  arg0;       // +0x08
	u32  arg1;       // +0x0C
	u32  fromId;     // +0x10, a language hash
} SmsRec;

// GetMailboxCategoryLanguageHash (Speed.elf 0x800DC84C), the whole function:
// six constants, one per category, and 0 for anything else. All six resolve in
// the shipped English.bin -- Misc, Inbox, Game Tips, Special Events, Rachel,
// Unlocks.
#define SMS_CATEGORY_COUNT 6
#define SMS_CAT_MISC_ID     0xB0819356u
#define SMS_CAT_INBOX_ID    0xC06E138Au
#define SMS_CAT_TIPS_ID     0xF8CB19A3u
#define SMS_CAT_EVENTS_ID   0x06F8E5BFu
#define SMS_CAT_RACHEL_ID   0xE2442059u
#define SMS_CAT_UNLOCKS_ID  0xEA33E5B6u

// GetSMSMessageDataByNameHash (0x8012B77C): a linear scan comparing
// bStringHash(GetMessageName(rec)) against the key, and 0 for a miss. Returns
// an index into `db->sms`, or -1.
// FEngHashString (Speed.elf 0x8014B6C4): djb2 over the upper-cased string.
// Every name the frontend addresses a widget or a message by goes through
// it, which is why `Body_Text_1` and `BODY_TEXT_1` are one hash.
u32 eventdb_feng_hash(const char* s);

typedef struct {
	EventRec recs[EVENTDB_MAX_EVENTS];
	int count;
	ShopRec shops[EVENTDB_MAX_SHOPS];
	int shopCount;
	StageRec stages[EVENTDB_MAX_STAGES];
	int stageCount;
	SponsorRec sponsors[EVENTDB_MAX_SPONSORS];
	int sponsorCount;
	SmsRec sms[EVENTDB_MAX_SMS];
	int smsCount;
	CarUnlockRec carUnlocks[EVENTDB_MAX_CAR_UNLOCKS];
	int carUnlockCount;
	ShowcaseRec showcases[EVENTDB_MAX_SHOWCASES];
	int showcaseCount;
	WorldChallengeRec challenges[EVENTDB_MAX_CHALLENGES];
	int challengeCount;
	BankTriggerRec banks[EVENTDB_MAX_BANK_TRIGGERS];
	int bankCount;
} EventDb;

// FindCareerBankTrigger (0x801233BC): the record whose zone hash (+0x08) is
// `zoneHash`, a BANK_TRIGGER zone's +0x30, or NULL.
const BankTriggerRec* eventdb_bank_by_zone(const EventDb* db, u32 zoneHash);

int eventdb_sms_find(const EventDb* db, u32 nameHash);

// One entry of the profile's event-progress table: SPEED2.EXE walks it as 8
// byte records at DAT_00862840 with the count in DAT_0086283C, reading the id
// at +0, a type byte at +4 and a state byte at +6.
//
// In retail that table is never populated. It sits in .data's zero-filled tail
// (raw size 0x35000, so everything past VA 0x81D000 is BSS) and a scan of every
// direct store form in .text -- C7 05, A3, 89 0D/15/1D/35/3D, C6 05, A2, FF 05,
// 3919 distinct globals between them -- finds not one write to either address,
// while the control case 0x8383E0/0x8383E4 shows exactly the one store 0x512C00
// makes. So `progCount` is 0 for now and the parameter exists so a save-game
// layer can fill it in without touching the query.
typedef struct {
	u32 eventId;
	u8 type;  // +4
	u8 pad;
	u8 state; // +6
	u8 pad2;
} EventProgress;

// Scans `bunPath` for the first top-level 0x80034A10 container and decodes its
// 0x34A11 and 0x34A12 sub-chunks. Reads only chunk headers plus the two
// payloads, so this never mallocs the whole ~8MB container -- same shape as
// trackdb_load(). Returns true if the event table loaded; the shop table is
// independent, and `shopCount` is 0 when the container has no 0x34A12.
bool eventdb_load(const char* bunPath, EventDb* out);

// FindCareerEventDataByHandle: the record whose id (+0x08) is `eventId`, or
// NULL. Which event a free-roam start point offers is not a table question --
// 65 of the shipped start points serve more than one event -- but the
// career's EventPile's (main.c, TASKS.md #339).
const EventRec* eventdb_event_by_id(const EventDb* db, u32 eventId);

// BuildRideForPlayer's one field (EventRec.presetCar, +0x2C) of event
// `eventId`, read straight off the container for the frontend, which picks
// the career's car before the world loads the table. 0 when the event names
// none, or is not there.
u32 eventdb_event_preset_car(const char* bunPath, u32 eventId);

// _get_career_event_indicator_type (Speed.elf 0x80111C1C), transcribed: the
// EventIndicatorType whose texture the Engage widget's tab carries. SUV and URL
// events answer for themselves before the type is consulted. Feed the result to
// engage_draw(); 0 is never returned, so the tab always has art.
int eventdb_event_indicator_type(const EventRec* ev);

// get_event_trigger_zone_type (0x80123770) followed by the minimap's own
// _map_trigger_zone_type_to_element_type (0x80114E88), transcribed: the
// MinimapElementType for this event, always 12..18. Those are exactly the types
// MinimapElement::Update (0x801156F4) drives with FEngSetTextureHash, which is
// why an event marker can pick its art at all where a shop marker cannot.
int eventdb_event_minimap_element(const EventRec* ev);

// get_shop_trigger_zone_type (Speed.elf 0x80123808) followed by
// _map_trigger_zone_type_to_element_type (0x80114E88), transcribed: the
// MinimapElementType for this shop, 6..11 -- crib, paint, parts, ICE,
// performance or car lot, which is all that tells the shop markers apart.
int eventdb_shop_minimap_element(const ShopRec* shop);

// FindShopDataDescByTriggerZone (Speed.elf 0x80121F54): the shop a free-roam
// SHOP_TRIGGER polygon opens, found by the handle at the zone's +0x30.
// NULL when the handle is not a shop's -- which is what the engine's own
// fall-through does, and what the 24th record (no trigger) would produce.
const ShopRec* eventdb_shop_by_trigger(const EventDb* db, u32 triggerHandle);

// FindShopDataDescByHandle (Speed.elf 0x80121F14): the shop whose +0x38 is
// `handle`, or NULL.
const ShopRec* eventdb_shop_by_handle(const EventDb* db, u32 handle);

// FindShowcaseDataByHandle (Speed.elf 0x801224D4) and
// FindShowcaseDataByTriggerZone (0x80122514): the showcase whose +0x20 /
// +0x2C is `handle`, or NULL.
const ShowcaseRec* eventdb_showcase_by_handle(const EventDb* db, u32 handle);
const ShowcaseRec* eventdb_showcase_by_trigger(const EventDb* db, u32 handle);

// FindWorldChallengeByTriggerZone (Speed.elf 0x80122E1C): the challenge whose
// start trigger's bStringHash is the zone's handle, or NULL.
const WorldChallengeRec* eventdb_challenge_by_trigger(const EventDb* db,
	u32 handle);

// GetEventPurse (Speed.elf 0x801275b0), transcribed: what winning this event
// pays, which is what both the engage dialog and GetLastPurse (CommitWinEvent's
// deposit) ask for.
//
// A sponsored event (class 1) does NOT use the purse in its own record. The
// engine takes the RuntimeCareerStageInfo of the career's *current* stage
// (PlayerCareerState +0x69C4 + stage * 0x104 -- the stage at +0x71E4, not the
// event's), reads the sponsor chosen there (+0xE8, PlayerCareerState +0x6AAC)
// and pays that SponsorData's +0x02; everything else takes the record's own
// +0x30. A stage with no sponsor chosen yet falls back to the event's purse,
// the engine's own null check. That is a real difference on the shipped table,
// where the 44 sponsored records carry 1000..10000 of their own and the
// sponsors pay 825..3225. `chosenSponsor` is that stage's sponsor name hash
// (what the save keeps), 0 for none.
u32 eventdb_event_purse(const EventDb* db, const EventRec* ev, u32 chosenSponsor);

// FindSponsorDataByName (Speed.elf 0x80122AEC): the sponsor whose name hashes
// to `nameHash`, NULL for 0 or none.
const SponsorRec* eventdb_sponsor_by_hash(const EventDb* db, u32 nameHash);

// FindStageDataByStage (Speed.elf 0x801230A0): a linear scan keyed on each
// record's own stage byte, NULL for none.
const StageRec* eventdb_stage(const EventDb* db, int stage);

// GetURLPurse (Speed.elf 0x80125F2C): the purse (+0x30) of the first URL
// event (class 2) of `stage` in table order, 0 when the stage has none.
u32 eventdb_url_purse(const EventDb* db, int stage);

// ---------------------------------------------------------------------------
// The engage race dialog's text
// ---------------------------------------------------------------------------
//
// RaceEngageDialog::Setup (Speed.elf 0x800E8570) fills seven widgets of
// UI_EngageRaceDialog.fng, and all seven of its object hashes resolve in the
// shipped screen to a widget of the right type -- six Strings and the one Image
// it hides. They are listed here with what each is given.
//
//   0x2C167533  "Subject: %s"  <- the race-type name, eventdb_engage_subject_id
//   0x15E1351B  the same family's short name, eventdb_engage_type_id
//   0xDE075B62  the body, eventdb_engage_body_label
//   0xFECED617  "From: %s"     <- senderStringId, else fromName
//   0x080C9DAA  "%$f %s"       <- track length / 1600, then "mi" or "km";
//                                 EA's %f with no precision is 3 places
//   0xB54B81A7  "%$d"          <- GetEventPurse, digit-grouped
//   0x0F365871  an Image, hidden outright
#define ENGAGE_DLG_SUBJECT_HASH  0x2C167533u
#define ENGAGE_DLG_TYPE_HASH     0x15E1351Bu
#define ENGAGE_DLG_BODY_HASH     0xDE075B62u
#define ENGAGE_DLG_FROM_HASH     0xFECED617u
#define ENGAGE_DLG_DISTANCE_HASH 0x080C9DAAu
#define ENGAGE_DLG_PURSE_HASH    0xB54B81A7u
#define ENGAGE_DLG_HIDE_HASH     0x0F365871u

// The two "Subject:" / "From:" frames and the two unit words, all four read
// straight out of Setup's own GetLocalizedString calls and all four confirmed
// against the shipped English.bin.
#define ENGAGE_DLG_SUBJECT_FMT_ID 0x17EA64F8u // "Subject: %s"
#define ENGAGE_DLG_FROM_FMT_ID    0x9F29D70Du // "From: %s"
#define ENGAGE_DLG_UNIT_MILES_ID  0xAC3A4F4Cu // "mi"
#define ENGAGE_DLG_UNIT_KM_ID     0xB5D640B1u // "km"

// Track length -> the distance the dialog prints. Setup converts TrackInfo+0x98
// to a double and divides by this exact constant (a float literal at
// Speed.elf 0x80323D50), so it is kept as one rather than rounded to a real
// metres-per-mile figure.
#define ENGAGE_DLG_DISTANCE_DIVISOR 1600.0f

// The race-type name the dialog's Subject line carries, and the shorter one
// under it. Both come from Setup's own two-register table, which picks by class
// first and type second; the defaults (plain "Circuit") are what a DDAY record
// and a circuit race both take. Every id below resolves in the shipped
// English.bin to the name it was decoded as -- "Sprint", "Street X", "Drag",
// "Drift", "URL"/"U.R.L.", "Sponsored Circuit" and so on, 17 for 17.
//
// Note Setup reaches that table through the negation of
// IsSUVRace__15CareerEventData (0x803036D4, `isSUV == 1`), taken inline at
// 0x800E879C -- so the first arm is the ordinary race table and SUV events
// share it, and only URL and sponsor events take the second.
u32 eventdb_engage_subject_id(const EventRec* ev);
u32 eventdb_engage_type_id(const EventRec* ev);

// GetEngageDialogBody (0x80121B88), transcribed. The body is not a fixed
// string: outside stage 0 the engine picks one of N written variants at random
// and looks it up by label, e.g. ENGAGE_SPON_DRIFT_RANDOM_7. `variant` is
// retail's own bRandom(N)+1, so it runs 1..N; pass the result of
// eventdb_engage_body_variants() through a random of the caller's choosing.
// Stage-0 records instead get one body of their own, ENGAGE_<event name>.
//
// Every label this builds was checked against the shipped English.bin: all 137
// of them resolve, and each family has exactly the number of variants the
// engine's own bound allows -- 18 circuit, 15 sprint, 14 street, 12 drag, 13
// drift, 12 URL, 9 SUV, and 11/10/10/8/11 for the five sponsored families.
int eventdb_engage_body_variants(const EventRec* ev);
u32 eventdb_engage_body_label(const EventRec* ev, int variant,
	char* out, size_t outSize);

// SPEED2.EXE:0x5143C0 -- is `trackId` unlocked? Walks every event whose track
// list mentions the track (the 0x4FFCF0 iterator) and returns true as soon as
// one of them passes 0x514340: the matching entry's flag bit 0x2 is set, or the
// event's id is in the progress table with state 1.
//
// Flag bit 0x2 is the "available from the start" mark, and the shipped table
// sets it on exactly eight tracks -- 4001 and 4014 (Circuit), 4123 (Sprint),
// 4201 (Drag), 4174 and 4312 (Drift), 4603 (Street X), 4701 (URL). One or two
// per race type, which is what a fresh profile's Quick Race offers.
bool eventdb_track_unlocked(const EventDb* db, u16 trackId,
	const EventProgress* prog, int progCount);
