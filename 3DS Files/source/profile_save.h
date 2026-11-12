// Real NFSU2 save-file reader/writer -- tasks #67/#82 (C1/C2/C3).
//
// This is the REAL PC on-disk format, not an approximation: an EA `MC02`
// container wrapping a 0xD692-byte `UserProfile` payload. Every constant
// below is transcribed from the decompiled `SPEED2.EXE` and verified
// byte-for-byte against the user's own retail save
// (`PC Files/SAVE/NFS Underground 2/Player/Player`, all three container
// checksums OK). The full derivation -- container layout, the non-standard
// CRC, the complete block-B field map, and the evidence for each decoded
// field -- is in `docs/formats/savefile.md` **Part 2**. Read that before
// touching this file.
//
// Part 1 of that document describes the *GameCube* serializer, which an
// earlier version of this file was built from. The two builds do not agree
// on field widths (several GameCube u32 flags are u8 on PC) and the PC build
// carries a 0x43E-byte DirectInput block the GameCube one does not, so the
// payload sizes differ (PC 54,930 vs GC 48,018). The PC build wins here:
// it is the build whose data files this port ships, and therefore the build
// whose save files a user can actually drop in.
//
// Undecoded regions are read back and written out verbatim / left at zero
// -- per this project's standing rule they are never guessed at. The
// decoded `Profile` struct below exposes only fields with a cited
// derivation.
//
// **Porting-layer decision, not decompiled**: the retail PC build stores
// saves under the user's Documents folder, which has no 3DS equivalent. This
// port reads them from `<GameDir>/SAVE/NFS Underground 2/<Name>/<Name>` --
// i.e. the same `SAVE/NFS Underground 2` tree the real game creates, rooted
// at the configured game directory instead of Documents. That mirrors the
// existing `buildGamePath()`/`GameDir` convention for assets and means a
// user can copy their real save directory onto the SD card unmodified. The
// *bytes inside* the file are the real format; only the root is a porting
// choice.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "options.h"

// ---- container (docs/formats/savefile.md Part 2, "Container: MC02") -------
#define PROFILE_MC02_MAGIC     0x4D433032u // 'MC02'
#define PROFILE_MC02_HDR_SIZE  0x1C
#define PROFILE_MC02_BLOCKA    0x08
#define PROFILE_MC02_DATA_OFF  (PROFILE_MC02_HDR_SIZE + PROFILE_MC02_BLOCKA) // 0x24

// UserProfile::GetSaveBufferSize (FUN_0052a440) = FUN_005b9680() + 0xd254
//                                               = 0x43e + 0xd254 = 0xD692.
// A compile-time constant on PC: every fixed-count array in the payload has
// a fixed on-disk size, so a brand-new profile's buffer is exactly as large
// as a fully-played one.
#define PROFILE_PAYLOAD_SIZE   0xD692
#define PROFILE_FILE_SIZE      (PROFILE_MC02_DATA_OFF + PROFILE_PAYLOAD_SIZE) // 54,966
// #235: the career save's whole second copy beside it, "<Name>/<Name>.mirror",
// rewritten in place before the save so a torn write leaves one whole copy.
#define PROFILE_MIRROR_EXT     ".mirror"

// A kept magazine or DVD cover's payload: the 512x512 A8R8G8B8 grab
// (SPEED2.EXE FUN_005C3120's StretchRect target), B, G, R, A per pixel, rows
// top down.
#define PROFILE_COVER_DIM      512
#define PROFILE_COVER_SIZE     (PROFILE_COVER_DIM * PROFILE_COVER_DIM * 4) // 0x100000

// ---- block-B slot counts --------------------------------------------------
#define PROFILE_NUM_STOCK_SLOTS   48
#define PROFILE_NUM_TUNED_SLOTS   20
#define PROFILE_NUM_CAREER_SLOTS   5
#define PROFILE_NUM_ONLINE_SLOTS   6
#define PROFILE_NUM_STATE_RECORDS 384
#define PROFILE_NUM_BANK_FLAGS    256

// How much of a record's PresetCarSlot array Profile::careerParts keeps:
// CarSlotId 0 (the car-name hash) through CABIN_NEON_FRAME 0x9f, which covers
// every body part, every paint, every vinyl/decal slot (savefile.md) and the
// five neon slots past them.
//
// The save itself always held all of this -- the on-disk array is u32[0x106]
// and the shortest record stride (0x430) has room for every one of them -- so
// raising this only changes how much of it is read.
//
// It runs to 0xAA, the last index FillWithRide (Speed.elf 0x80134F38) writes:
// the car-name hash, then SetInstalledPart for each of RideInfo's 0xAA part
// slots one index up. That is all of a record the car lot fills, so a car it
// sells can be written back whole.
#define PROFILE_CAR_SLOTS 0xAB

// ePhysicsPerformanceParts runs 0..67, so a PhysicsUpgradeSpecification keeps
// 68 installed bytes and an OwnedPartInventory 68 brand slots. The same count
// as perfdb.h's PERFDB_NUM_PARTS; it is repeated rather than included so the
// save layer keeps standing on its own.
#define PROFILE_NUM_PERF_PARTS 68
// OwnedPartInventory's saved block (REC_CAREER_INVENTORY_N in profile_save.c).
#define PROFILE_INV_BLOCK_SIZE 0x3C0
// The 20 floats the same spec opens with: the upgrade level of each path.
#define PROFILE_NUM_UPGRADE_PATHS 20

// The career arrays below hold one slot past the save's five: PROFILE_WORK_SLOT,
// the car the garage edits when the main menu's CUSTOMIZE opened it
// (BeginCarCustomize entry point 3, TASKS.md #379). PORT: retail's garage edits
// the tuned FECarConfig itself; this port's garage is written against the career
// arrays, so the tuned car is copied in, edited there and copied back
// (profile_tuned_to_work / profile_work_to_tuned). Nothing reads or writes the
// work slot to the card.
#define PROFILE_WORK_SLOT      PROFILE_NUM_CAREER_SLOTS
#define PROFILE_CAREER_ARRAYS  (PROFILE_NUM_CAREER_SLOTS + 1)

// Profile::careerCarWrite bits.
//   FILLED: FECarConfig::FillFromStockCar (Speed.elf 0x80135388) ran on the
//           slot -- its PhysicsUpgradeSpecification is a fresh one (all zero:
//           the RidePhysicsInfo ctor 0x801C7D08 sets every upgrade level to
//           0.0, every junkman flag to 0 and clears the installed bytes) and
//           DefaultCustomTuningSpecs (0x80135004) puts its five tuning blocks
//           back to 100s.
//   TRADED: CareerCarTrade (0x80133FF4) ran Default__18OwnedPartInventory
//           (0x80135A28) on the slot first, emptying the parts bought for the
//           car that was there.
#define PROFILE_CAREER_WRITE_FILLED 0x01
#define PROFILE_CAREER_WRITE_TRADED 0x02

// A car record's custom tunings: PresetCarSlot +0x348 (record +0x35C), five
// 0x29-byte CustomPhysicsTunings, one per eTuningSetting in StartCarTuningRace's
// order (0 Circuit, 1 Drag, 2 Drift, 3 Street X, 4 URL; custtune.h). Each byte
// is one tuning type's slider, 100 the car's own value (DefaultCustomTuningSpecs,
// 0x80135004, fills all five with 100s).
#define PROFILE_NUM_TUNING_SETTINGS 5
#define PROFILE_TUNING_SIZE         0x29
#define PROFILE_TUNING_DEFAULT      100

// The name DefaultCareerCars (Speed.elf 0x801340A4) gives career slot n,
// FEngHashString("CAREER_SLOT_%d") -- 0x319A8CA1 + n.
unsigned int profile_career_slot_hash(int slot);

// FECarRecord::MatchesFilter (80135198) filter bits.
#define PROFILE_FILTER_STOCK   1
#define PROFILE_FILTER_TUNED   2
#define PROFILE_FILTER_CAREER  4
#define PROFILE_FILTER_ONLINE 16

#define PROFILE_NAME_MAX 31  // 32-byte on-disk field, always NUL-terminated
#define PROFILE_PATH_MAX 256

// One `FECarRecord`-derived slot. The first five u32 are the shared
// FECarRecord base (disk +0x00..+0x10); the sixth word in memory is the
// vtable pointer and is never serialized. `carType` is only meaningful for
// stock slots (FEStockCar disk +0x14, an index into the car-data table);
// `carTypeHash` only for tuned/career/online slots (FECarConfig disk +0x1C,
// an EA string hash of the car name).
typedef struct {
	unsigned int  nameHash;    // +0x04 "STOCK_<CAR>" / "CAREER_SLOT_<n>" / "ONLINE_CAREER_<MODE>"
	float         perf;        // +0x08 GetPerformanceMeasure__15RidePhysicsInfo, 0 until computed
	unsigned int  inUse;       // +0x0C set to 1 by FillWithData/DefaultCareerCars/FillFromStockCar
	unsigned int  filter;      // +0x10 PROFILE_FILTER_*
	unsigned int  carType;     // stock only
	unsigned int  carTypeHash; // tuned/career/online only
	// +0x14 on tuned/career/online records, PresetCarSlot's first word, which
	// FillWithRide (0x80134F38) sets to 1 as it fills the slot in. 0 on a
	// career slot that has never held a car.
	unsigned int  hasRide;

	// Career slots only, bytes at record +0x430 / +0x431 (PC DefaultCareerCars
	// 0x5164A0 writes them as bytes at object +0x7EC / +0x7ED; GC loads them as
	// words at +0x7F4 / +0x7F8). BuildListOfCars (GC 0x800D59AC) reads them as
	// the crib car select's three row states: locked, open for a sponsor car
	// (neither), or a real car (chosen). The user's save: slots 0-1 0/1, slot 2
	// 0/0, slots 3-4 1/0.
	unsigned char locked;
	unsigned char chosen;

	// Customisation, out of the record's PresetCarSlot array (see
	// PROFILE_SLOT_* below). Each is the bStringHash of a *part name*, not a
	// value: the colours resolve through GLOBALB's car-parts database, which
	// tools/paint_pack.py bakes into PAINT.nfsp. Zero means the slot is empty,
	// which for a paint slot means "same as the body".
	unsigned int  basePaint;      // PROFILE_SLOT_BASE_PAINT
	unsigned int  paintRoof;
	unsigned int  paintSpoiler;
	unsigned int  paintMirror;
	unsigned int  paintRim;
	unsigned int  paintSpinner;
	// The four vinyl layers. Kept because they are decoded and belong with the
	// rest of the record, but nothing draws them yet: NFSU2's vinyl art is not
	// a bitmap anywhere in the retail tree, it is vector shape data, so a
	// rasteriser has to come first (TASKS.md).
	unsigned int  vinylLayer[4];

	// Neon. Four colour slots and a style, all part-name hashes like the
	// paints above; carneon.c resolves the colours through GLOBAL/NEON.nfsn.
	// A car that has visited the shop has these filled even when it wears
	// none -- the "off" value is the NO NEON part, not a zero (neon_pack.py).
	//
	// Only `neon` can show (carneon.h has the decode). The engine-bay and boot
	// tubes are faded by their panel's opening animation, zero with it shut,
	// and nothing in the world opens either. The cabin tubes are never lit at
	// all: RenderNeon reads their colour from a word Render never fills.
	unsigned int  neon;           // PROFILE_SLOT_NEON
	unsigned int  neonEngine;
	unsigned int  neonCabin;
	unsigned int  neonTrunk;
	unsigned int  cabinNeonFrame; // the style, not a colour: how many cabin tubes
} ProfileCarRecord;

// The decoded subset of a UserProfile. Only fields with a cited derivation
// in savefile.md Part 2 appear here; everything else in the payload is left
// untouched in the raw buffer.
// One entry of the career's event-progress table, 8 bytes, laid out as the
// save file carries it. The two pad bytes are real record bytes this port does
// not decode, kept so the struct is the record rather than a summary of it.
typedef struct {
	unsigned int  eventId; // +0x00, the event's name hash
	unsigned char type;    // +0x04
	unsigned char pad;     // +0x05
	unsigned char state;   // +0x06, 1 = completed
	unsigned char pad2;    // +0x07
} ProfileStateRecord;

// RuntimeCareerStageInfo records, 8 of them at payload 0x993E, stride 0x101
// (savefile.md), indexed by stage number. The completed byte (+0xFC, the byte
// FUN_00500FA0 copies as +0x3F words) is PlayerCareerState::HasCompletedStage,
// which CribWorldMapScreen::HasFinishedStage (Speed.elf 0x800D946C) calls to
// decide which stages the world map lists; the rest is the stage's sponsor
// bookkeeping, which SaveToBuffer (0x8012A420) writes field by field in this
// order. The PC record is GC's 0x104 less three bytes: +0xFC is one byte, so
// the stage number lands at +0xFD.
#define PROFILE_NUM_STAGES 8
#define PROFILE_STAGE_WORLD_RACES 35
#define PROFILE_STAGE_SPONSOR_RACES 4
#define PROFILE_STAGE_URL_RACES 10
#define PROFILE_STAGE_SPONSORS 5
#define PROFILE_STAGE_SHOWCASES 6

typedef struct {
	// +0x00 / +0x8C / +0x9C: the event ids won this stage, by class --
	// NotifyWonEvent (0x801247C4) puts a world race (class 3, not an SUV
	// race), a sponsor race (1) or a URL event (2) in the first slot that is
	// empty or already holds it. The Get*RacesCompleted counts are the
	// non-zero slots.
	unsigned int worldRace[PROFILE_STAGE_WORLD_RACES];
	unsigned int sponsorRace[PROFILE_STAGE_SPONSOR_RACES];
	unsigned int urlRace[PROFILE_STAGE_URL_RACES];
	// +0xC4: the showcases done while this stage was the career's, by handle
	// -- RuntimeCareerStageInfo::NotifyShowcaseCompleted (SpeedPS.elf
	// 0x80124730) puts one in the first slot that is empty or already holds it.
	unsigned int showcase[PROFILE_STAGE_SHOWCASES];
	// +0xE0 / +0xE4: the sponsor points won this stage and the races they
	// came from (NotifyCommitSponsorPoints, 0x80124B08), whose quotient is
	// GetAverageSponsorPointsPerRace -- 0 while the count is 0.
	float        sponsorPoints;
	int          sponsorPointRaces;
	// +0xE8: bStringHash of each sponsor name, 0 for none. Only [0] is ever
	// written (NotifyChooseSponsor, 0x80124B00): the stage's chosen sponsor.
	unsigned int sponsor[PROFILE_STAGE_SPONSORS];
	// +0xDC: the outrun races won this stage, the counter MaybeAddWorldChallenge
	// (0x80125104) and NotifyOutrunRaceFinished (0x8012CE38) compare a uniques
	// challenge's +0x07 against. Raised on each Outrun won
	// (NotifyRandomEncounterFinished, #268 H step 2) and written back.
	int          outrunWins;
} ProfileStageInfo;

// HighScoresDatabase, payload 0xC731 (0xA50 bytes): its first 320 records,
// 8 bytes each. SPEED2.EXE 0x502AA0 (best time) and 0x502C50 (best score)
// walk all 320 matching the track id and kind and ignore +0x02.
#define PROFILE_NUM_HIGHSCORES 320
typedef struct {
	short         trackId;  // +0x00
	unsigned char category; // +0x02, unread by both lookups
	unsigned char kind;     // +0x03, 1 = reversed (0x4FFDC0)
	int           value;    // +0x04, 1/4000 s ticks, or points
} ProfileHighScore;

// One SMSMessageQueue slot: 256 of these 0x14-byte records at save offset
// 0xAE76, then the queue's own three u32s (savefile.md).
//
// The layout is SMSMessageQueue::SaveToBuffer's (Speed.elf 0x8012CAD8), which
// writes the fields one save_bytes() call at a time and so names each width
// outright -- u32, then four single bytes, then three u32s. It is the live
// SMSMessage struct written out whole, which is why GetSubject (0x8012BA10)
// and GetBody (0x8012BB04) read members at the same offsets.
//
//   +0x00 u32  bStringHash of the message's name. The pointer a live
//              SMSMessage holds here is resolved back on load through
//              GetSMSMessageDataByNameHash; it stays a hash in this port and
//              eventdb_sms_find() is the lookup, so the save parser needs no
//              career table to run.
//   +0x04 u8   read     -- NotifyReadMessage (0x8012C588)
//   +0x05 u8   popped   -- MarkAllAutoPopupsSeen (0x8012BCC4)
//   +0x06 u8   deleted  -- NotifyDeleteMessage (0x8012C610)
//   +0x07 u8   sortable -- MarkAllReadMessagesSortable (0x8012C3F4). Until it
//              is set the message shows only in the Inbox; after it, only in
//              its own category (CanDisplayMessageInCurrentList 0x800DD1DC).
//   +0x08 u32  body frame, a language hash. When it is non-zero GetBody uses
//              it as a format and feeds `arg` through it instead of looking up
//              a <NAME>_BODY label.
//   +0x0C u32  the argument both frames take, itself a language hash.
//   +0x10 u32  subject frame, the same arrangement in GetSubject.
//
// The three are zero for every message raised the ordinary way -- BuildFrom
// (0x8012B9C8) clears them -- and set only by MakeMagazineMessage
// (0x8012B9F4), which is how AddMagazineMessage raises the SMS_MAG_* and
// SMS_UNIQUE_* families. That is also why those 28 names have no _SUBJECT or
// _BODY label in English.bin: they are never looked up by name.
#define PROFILE_MAX_SMS 256

// CareerSpeech's saved block, 0x190 bytes on PC (the GameCube's is 0x1D8:
// the same fields, its flags ints rather than bytes).
#define PROFILE_SPEECH_BLOCK 0x190

typedef struct {
	unsigned short slot;  // which of the 256 disk slots this came from, so the
	                      // flags can be written back without re-scanning
	unsigned int id;      // +0x00
	unsigned char read;   // +0x04
	unsigned char popped; // +0x05
	unsigned char deleted;// +0x06
	unsigned char sortable;// +0x07
	unsigned int bodyFrameId;    // +0x08
	unsigned int argId;          // +0x0C
	unsigned int subjectFrameId; // +0x10
} ProfileSmsSlot;

typedef struct {
	char name[32];

	unsigned int cash;            // CareerWallet balance (PlayerCareerState +0x7134)
	// +0x872c: the races entered. NotifyEnterEvent (SPEED2.EXE 0x541160) adds
	// one on every event start; FillStatistics (0x528A60) reads it for the
	// status screen's "Races Entered" and the win ratio.
	unsigned int racesEntered;
	// Body 0x009 and 0x00A (GC PlayerCareerState +0x7E30 and +0x7E34, saved
	// between the races entered and the stage there too), the two flags
	// BuildListOfEvents (Speed.elf 0x80125A30) gates stage 0's races on:
	// csCanSeeEvents opens them -- NotifyCSCanSeeEvents (0x8012AE4C), from
	// CareerSpeech's event 2, 15 s into DDAY_EVENT_A -- and stage0Timeout
	// closes them again -- NotifyCSStage0Timeout (0x8012AE1C), from the third
	// stage 0 race's end. Both clear on Default (0x80123934).
	bool stage0Timeout;
	bool csCanSeeEvents;
	unsigned int careerStage;     // +0x7af8, 0..7
	unsigned int numStateRecords; // +0x7afc
	unsigned int numCompleted;    // non-empty StateRecords actually present
	unsigned int stateRecCount;   // live entries in stateRec[], see below
	unsigned int numBankPickups;  // set bytes in the 256-entry claimed-flags table
	// The table itself (body 0xC30, GC PlayerCareerState +0x7E58): one byte
	// per CareerBankTrigger flag index, set by ClaimBankReward (#293).
	unsigned char bankFlags[PROFILE_NUM_BANK_FLAGS];
	unsigned int careerSettings;  // payload 0xC723
	// The Autosave option: `autosave` is "save when the career moves on"
	// (payload 0xC72C, UserProfile +0xA0F8), `autosaveNoPrompt` is "without
	// asking first" (0xC72B, +0xA0F9). Off, Prompt and Auto are 0/0, 1/0, 1/1.
	bool autosave;
	bool autosaveNoPrompt;
	// The in-game world map's legend filters (payload 0xC27F, GC
	// PlayerCareerState +0x7F5C), one byte per trigger-zone type, 1 = shown.
	// PlayerCareerState::Default sets all sixteen; GameWorldMapScreen's
	// ToggleFilter flips them and MinimapElement::IsRenderable reads them.
	unsigned char mapFilter[16];
	// CareerSpeech, PlayerCareerState +0x887C on PC, saved whole right after
	// the filters (payload 0xC28F): PlayerCareerState::SaveToBuffer (SPEED2.EXE
	// 0x514530) and LoadFromBuffer (0x529230) copy its 0x190 bytes raw. main.c
	// reads it into its CareerSpeech struct and writes that back before each
	// save (careerSpeechLoadBlock / careerSpeechStoreBlock).
	unsigned char speechBlock[PROFILE_SPEECH_BLOCK];
	// OptionsSettings' four blocks, stored whole at payload 0xC4FB (options.h
	// has the layout and the offsets).
	ProfileOptions options;

	// +0x873c (GC PlayerCareerState+0x7e28, DAT_803bbecc), the byte before
	// hasCareerCar: whether the career has started. Clear on a new profile
	// (Default, Speed.elf 0x80123934); InGamePlayMovie::DoWhatNext (0x80183E80)
	// sets it once the first career movie has played. UIStartCareer::Setup
	// (0x800D1F7C) reads it to choose between Resume Career and Start New
	// Career, and StartCareerHelper (0x800D1B30) to play that movie.
	bool         careerStarted;

	unsigned int currentCarHash;  // +0x8740, a CAREER_SLOT_<n> record hash
	int          currentCarSlot;  // index into careerCar[], or -1
	// +0x873d (GC PlayerCareerState+0x7e2c), the flag byte just before it:
	// whether the career has a car of its own. Retail's main menu shows
	// DDAY_PLAYER_CAR while it is clear (UIMain, Speed.elf 0x800d1a14; PC
	// MainFNGObject, byte [0x86347D]), BuildListOfEvents only asks the
	// current car IsSUV while it is set, and on career stage 0 it picks the
	// free-roam scenery group (InitTopologyAndSceneryGroups, 0x80196cb8).
	bool         hasCareerCar;
	// +0x8868 (GC PlayerCareerState+0x7f58), payload 0xA14A: set by
	// NotifyEnterShopFromFreeRoam (Speed.elf 0x80126DB0) the first time the
	// career drives into a shop with an event under way, cleared only by
	// Default (0x80123934). PauseMenu::NeedToSeeJumpToGarage (0x801000E8; PC
	// builder 0x4C29D1, byte [0x8635A8]) shows the pause menu's Jump to Garage
	// row while it is set.
	bool         jumpToGarage;

	float          respawnPos[3]; // +0x8750
	unsigned short respawnHeading;// +0x8760, 0..0xFFFF = a full turn
	// +0x8744 (GC PlayerCareerState+0x7E3C), payload 0xAD5F (body 0xC19): the
	// respawn point is good. Default (0x80123934) clears it. These set it:
	// NotifyExitToMainMenuFromFreeRoamOrRace (0x80126718),
	// NotifyEnterFreeRoamFromPause (0x80126674), NotifyEnterCribFromPauseMenu,
	// NotifyEnterShopFromFreeRoam (0x80126DB0),
	// NotifyEnterFEMagazineFromFreeRoam and NotifyEnterFreeRoamFromPostRace.
	// StartCareerFreeRoam (0x8011CBAC) reads the respawn only while it is set,
	// and otherwise puts the car on track 4000's grid (TASKS.md #366).
	bool           respawnValid;
	// The career clock, seconds: PlayerCareerState::UpdateTick (SPEED2.EXE
	// 0x551320, SpeedPS.elf 0x80127688) adds the world's time step while the
	// player drives career free roam, and SMSMessageQueue::Tick's trigger 3
	// reads it. Default (0x532830) sets -1.
	float          careerTime;    // +0x8764

	// SMSMessageQueue, payload 0xAE76. Both are recounted from the 256 slots
	// rather than read from the two summary fields the block ends with,
	// because retail recomputes them on load too and never trusts the file
	// (savefile.md, SMSMessageQueue). A message is unread when its read byte
	// is 0 and its deleted byte is not 1 -- GetNumMessagesUnread, Speed.elf
	// 0x8012c36c. Both counts match the file's own summary fields in the
	// user's save (55 queued, 1 unread).
	unsigned int smsCount;        // non-empty slots
	unsigned int smsUnread;
	// The queue itself, compacted the way the loader does -- empty slots
	// dropped, order kept (savefile.md). The mailbox needs the slots and not
	// just the counts, and 256 of them is 3 KB, so they are kept whole rather
	// than re-read off the card when the screen opens.
	ProfileSmsSlot sms[PROFILE_MAX_SMS];

	unsigned int numStockCars;    // FEPlayerCarDB +0x480
	unsigned int numCareerCars;   // FEPlayerCarDB +0x812c
	unsigned int numOnlineCars;   // FEPlayerCarDB +0x9a80

	ProfileCarRecord stockCar[PROFILE_NUM_STOCK_SLOTS];
	ProfileCarRecord tunedCar[PROFILE_NUM_TUNED_SLOTS];
	ProfileCarRecord careerCar[PROFILE_CAREER_ARRAYS];
	ProfileCarRecord onlineCar[PROFILE_NUM_ONLINE_SLOTS];

	// Each career car's slot array whole, indexed by CarSlotId: the part picks
	// carkit.c builds the car out of (TASKS.md #204). Only the career cars,
	// because they are the ones that are driven; the rest would be 24 KB of a
	// struct that lives on the stack while it is loaded.
	unsigned int careerParts[PROFILE_CAREER_ARRAYS][PROFILE_CAR_SLOTS];

	// The tuned cars' halves the garage edits (TASKS.md #379): each FECarConfig's
	// PresetCarSlot array whole, and its PhysicsUpgradeSpecification's path
	// levels and fitted bytes -- the same blocks a career record carries at the
	// same record offsets (FECareerCar opens with an FECarConfig). A tuned car
	// has no OwnedPartInventory: Quick Race's garage owns nothing.
	unsigned int  tunedParts[PROFILE_NUM_TUNED_SLOTS][PROFILE_CAR_SLOTS];
	unsigned char tunedInstalledParts[PROFILE_NUM_TUNED_SLOTS][PROFILE_NUM_PERF_PARTS];
	float         tunedUpgradePath[PROFILE_NUM_TUNED_SLOTS][PROFILE_NUM_UPGRADE_PATHS];
	// Each tuned car's five tuning blocks (PROFILE_TUNING_SIZE, record +0x35C).
	unsigned char tunedTuning[PROFILE_NUM_TUNED_SLOTS][PROFILE_NUM_TUNING_SETTINGS]
		[PROFILE_TUNING_SIZE];
	// PROFILE_CAREER_WRITE_FILLED per tuned slot: AddNewTunedCar's
	// FillFromStockCar ran (the spec zeroed, the tuning blocks to 100s), or
	// DeleteCurrentCar emptied it; profile_save_career() writes those resets.
	unsigned char tunedCarWrite[PROFILE_NUM_TUNED_SLOTS];

	// The performance half of a career record: which parts the car owns, which
	// of them are fitted, what the upgrade paths read, and which brands have
	// been bought from. Together they are everything perfdb.h's PerfUnlockCtx
	// needs, which is what decides the performance shop's prices and what it
	// will sell (TASKS.md #288).
	//
	// Two blocks of the record carry it, and neither is laid out the way the
	// struct is -- OwnedPartInventory::SaveToBuffer (Speed.elf 0x80135AE8)
	// writes its fields out of order, so the brand list that lives at object
	// +0x2AC is written at +0x2A0 of the block and the spend that lives at
	// +0x3BC is written at +0x294. The offsets below are that writer's, field
	// by field, and were checked against the user's own retail save: for every
	// career slot the fitted-part set is exactly the owned set plus the junkman
	// parts, and slot 2 -- which has bought performance parts and nothing else
	// -- carries a spend of $7000, the shipped price of its twelve parts summed.
	//
	//   ownedParts     block +0x15C, 16 bytes, bit (id & 7) of byte (id >> 3):
	//                  OwnedPartInventory::OwnsPerfPart (0x801359A8).
	//   ownedBrands    block +0x2A0, 68 u32 brand hashes, first zero ends the
	//                  list: OwnsBrand (0x801359E8).
	//   partsSpend     block +0x294: what has been spent on this car's parts,
	//                  performance and visual both -- PlayerCareerState::
	//                  BuyPerfPart and ::BuyCarPart each add to it, and
	//                  CareerStatisticScreen::Setup is what shows it.
	//   installedParts PhysicsUpgradeSpecification +0x50, one byte an
	//                  ePhysicsPerformanceParts: IsPerfPartInstalled
	//                  (0x801C4270).
	//   upgradePath    the same spec's first 20 floats, the levels the physics
	//                  reads.
	unsigned char careerOwnedParts[PROFILE_CAREER_ARRAYS][16];
	unsigned char careerInstalledParts[PROFILE_CAREER_ARRAYS][PROFILE_NUM_PERF_PARTS];
	unsigned int  careerOwnedBrands[PROFILE_CAREER_ARRAYS][PROFILE_NUM_PERF_PARTS];
	unsigned int  careerPartsSpend[PROFILE_CAREER_ARRAYS];
	float         careerUpgradePath[PROFILE_CAREER_ARRAYS][PROFILE_NUM_UPGRADE_PATHS];
	// OwnedPartInventory's visual half (#371), the block's bytes as
	// SaveToBuffer (0x80135AE8) lays them: the per-entity words (+0x000..
	// +0x15C), the rims, roof, spoiler and mirror runs (+0x16C..+0x294), the
	// hood's (+0x298, object +0x294) and slot 0xA2's (+0x3B0, object +0x29C).
	// The perf bits, spend and brands it also holds are the fields above, which
	// are the ones written back.
	unsigned char careerVisualInv[PROFILE_CAREER_ARRAYS][PROFILE_INV_BLOCK_SIZE];

	// Each career car's five tuning blocks (PROFILE_TUNING_SIZE bytes, record
	// +0x35C): the one for the race's eTuningSetting is what
	// ridephys_build_tuned() builds the car with (PresetCarSlot::FillRideWith,
	// 0x80134D4C, copies it into the RideInfo). Written back in place;
	// FillFromStockCar puts all five back to PROFILE_TUNING_DEFAULT.
	unsigned char careerTuning[PROFILE_CAREER_ARRAYS][PROFILE_NUM_TUNING_SETTINGS]
		[PROFILE_TUNING_SIZE];

	// What a career record needs beyond the fields above when it is written
	// back, PROFILE_CAREER_WRITE_* bits per slot: the car lot marks the slots
	// whose spec and inventory it has reset, and profile_save_career() writes
	// those resets (the tuning blocks are reset in careerTuning as well, and
	// written from there).
	unsigned char careerCarWrite[PROFILE_CAREER_ARRAYS];

	// The career's own event-progress table, kept whole rather than counted.
	// GetStateRecord__17PlayerCareerStateUi (Speed.elf 0x801238C0) walks the
	// first `count` entries of this array comparing the id at +0x00, and
	// HasCompletedEvent (0x801271DC) then reads +0x06 and treats exactly 1 as
	// completed -- so an event missing from the table has not been done, and a
	// state byte that is neither 0 nor 1 is not "completed" either. The id is
	// the event's name hash, which is what ties this table to the career event
	// table in eventdb.h; all 224 shipped events hash to their own id.
	//
	// Entries past `stateRecCount` are untouched file bytes and must not be
	// read: retail appends with AddStateRecord and never compacts.
	ProfileStateRecord stateRec[PROFILE_NUM_STATE_RECORDS];

	// Each stage's completed byte (RuntimeCareerStageInfo +0xFC). The user's
	// save "Player" has stages 0..2 completed and 3..7 not.
	bool stageCompleted[PROFILE_NUM_STAGES];
	// The rest of each stage's record. "Player": stage 1 five world races and
	// 1200 points from 5, stage 2 TOYO and 3500 from 17, stage 3 BRIDE and
	// 2600 from 13.
	ProfileStageInfo stage[PROFILE_NUM_STAGES];

	// The HighScoresDatabase records, kept whole; profile_best_time() and
	// profile_best_score() are the two scans retail runs over them.
	ProfileHighScore highScores[PROFILE_NUM_HIGHSCORES];

	// False when this is the built-in default rather than a file that was
	// actually read off the SD card (see profile_default_player1).
	bool isLoaded;
	char sourcePath[PROFILE_PATH_MAX];
} Profile;

// One entry from profile_list(): the on-disk profile name and the path the
// name was read from, ready to hand straight back to profile_load_file().
typedef struct {
	char name[32];
	char path[PROFILE_PATH_MAX];
	bool legacy; // true = a pre-C1 sdmc:/nfsu2data/profiles/*.sav from this port
} ProfileEntry;

// bStringHash -- h = 0xFFFFFFFF, then h = h * 33 + c for every byte.
// Verified against the shipped hash dump (TIBURON -> 0x04FD0522) and against
// the car-type hash in CAREER_SLOT_0 of the user's own save
// (CORSA -> 0x0293FAD7, a name the dump does not contain).
unsigned int profile_ea_hash(const char* s);

// Points the save layer at `sdmc:<GameDir>/SAVE/NFS Underground 2`. Call
// once after the config is loaded; until then the layer falls back to
// `sdmc:/nfsu2data/SAVE/NFS Underground 2`.
void profile_set_save_root(const char* sdmcSaveRoot);
const char* profile_save_root(void);

// Reads one real save file: validates the MC02 container exactly as
// FUN_006fd2f6 does (magic, file length, three CRCs) and decodes block B.
// Returns false and logs the specific failure on any mismatch -- a save that
// would be rejected by the real game is rejected here too.
bool profile_load_file(const char* path, Profile* out);

// Enumerates saves. Scans the real `<root>/<Name>/<Name>` directory layout
// first, then the legacy `sdmc:/nfsu2data/profiles/*.sav` files earlier
// builds of this port wrote (kept so an existing on-card profile still shows
// up; those carry only a name, no career data). Returns the number found.
int profile_list(ProfileEntry* out, int maxEntries);

// Deletes a listed profile: its save directory (save, .bak, .mirror,
// .display) and its collected covers' directories, as MemoryCard::Delete /
// BuildDeleteList do. True when the save directory went.
bool profile_delete(const ProfileEntry* e);

// Fills `out` with the default profile used when nothing has been loaded.
// Per the project directive this is "Player 1": a real, empty UserProfile
// with no career progress -- not a fabricated save state.
void profile_default_player1(Profile* out);

// Creates a brand-new save at `<root>/<name>/<name>`: a 0xD692 payload
// carrying the profile name, Default__11UserProfile's options, career
// difficulty and Autosave setting, and Default__17PlayerCareerState's career
// cars (five CAREER_SLOT_<n> records, slot 0 open) and current-car name,
// wrapped in a real MC02 container with real checksums. Every other field
// stays at zero, which is this project's rule for anything whose fresh-profile
// default has not been decompiled. `startCash` is PlayerCareerState::Default's
// wallet, the cheat codes' bonus (0x576090, #446).
bool profile_save_create(const char* name, unsigned startCash);

// Writes the career back into the save `p` was loaded from: the wallet, the
// career stage and each stage's completed byte, the event state records, the
// spawn point, the career cars (every record field the port keeps, plus the
// tuning and inventory resets careerCarWrite asks for), each stage's sponsor
// record (its won races, sponsor points and chosen sponsor), and the options
// (the four settings blocks, the career difficulty and the two autosave
// bytes), with the MC02 checksums redone.
// Everything else in the file is left byte for byte. The first write keeps a
// `<save>.bak` of the file as it was; the new file goes to `<save>.tmp` and
// replaces the save only once it is fully written. False, having logged why,
// when the save on the card no longer validates or cannot be written.
// The careerCarWrite bits are cleared once the file is written.
bool profile_save_career(Profile* p);

// FEPlayerCarDB's tuned cars (TASKS.md #379).
// AbleToAddNewTunedCar: the first of the 20 FECarConfig slots whose +4 (the
// name hash) is 0, or -1 when all are taken.
int  profile_tuned_free_slot(const Profile* p);
// AddNewTunedCar + FillFromStockCar: slot `slot` becomes TUNED_CAR_<slot>, in
// use, filter MY_CARS, holding `parts` (the stock car's slot array) with a fresh
// performance spec. False for a bad slot.
bool profile_tuned_add(Profile* p, int slot, const unsigned int parts[PROFILE_CAR_SLOTS],
	float perf);
// DeleteCurrentCar for a tuned slot: the record emptied.
void profile_tuned_delete(Profile* p, int slot);
// The garage's copies: a tuned slot into PROFILE_WORK_SLOT (owned nothing, spent
// nothing) and back.
void profile_tuned_to_work(Profile* p, int slot);
// A preset ride (presetcar.h, PresetCarSlot `parts`, [0] the car's name hash)
// into PROFILE_WORK_SLOT as a stock-performance car: Quick Race's Sponsor view
// (#445), whose presets all carry performance package 0.
void profile_preset_to_work(Profile* p, const unsigned int parts[PROFILE_CAR_SLOTS]);
void profile_work_to_tuned(Profile* p, int slot);

// MemoryCard's save of a kept magazine or DVD cover (mode 0xCB1, SPEED2.EXE
// FUN_00523EF0 -> FUN_004D9C60): `<root>/<title>/<title>`, `title` being
// "<profile> Magazine N" or "<profile> DVD N", beside the profile as the PC
// keeps them. The MC02 container's block A is {0x10D, 0x100000}; `bgra` is
// the PROFILE_COVER_SIZE payload. A cover already there is replaced. False,
// having logged why, when it cannot be written.
bool profile_save_cover(const char* title, const unsigned char* bgra);

// Its load (mode 0xCC1, UIMagazineView's 0x35F8620B arm, SPEED2.EXE
// FUN_00565B50): the same file, validated as any MC02 load is with a payload
// of PROFILE_COVER_SIZE. Returns that payload (B, G, R, A, rows top down) in a
// buffer the caller frees, or NULL, having logged why -- the screen's
// 0x2B46FE08 arm.
unsigned char* profile_load_cover(const char* title);

// ChooseCarForSlot (Speed.elf 0x801348D8) on career slot `slot`, the end of
// both of the car lot's commits: FillFromStockCar (0x80135388) -- in use,
// RidePhysicsInfo's stock measure as the record's perf, FillWithRide's
// PresetCarSlot `parts` (parts[0] is the car's name hash) -- then chosen = 1
// and filter 4. `traded` is CareerCarTrade's (0x80133FF4) OwnedPartInventory
// Default ahead of it. The record's tuning and inventory resets are written
// with the next profile_save_career(). Which slot is the current car is the
// caller's: CommitChooseCar sets it, a trade leaves it.
void profile_career_choose(Profile* p, int slot, const unsigned int parts[PROFILE_CAR_SLOTS],
	float perf, bool traded);

// SPEED2.EXE 0x502AA0's scan: the smallest value over the high-score records
// matching (trackId, kind). False when none match, which is where retail
// drops to the track's par time (trackdb_best_time_ticks).
bool profile_best_time(const Profile* p, int trackId, int kind, int* out);

// SPEED2.EXE 0x502C50's scan: the largest value over the matching records,
// starting from 0. Retail treats 0 as a miss and drops to the par score
// (trackdb_best_score).
int profile_best_score(const Profile* p, int trackId, int kind);

// eHighScoresRaceTypes -- what a track's race-type mask makes it, for the
// high-score table. CalcRaceTypeForHighScores (Speed.elf 0x801322FC) reads
// TrackInfo +0x94 and takes the FIRST bit set, in this order, so a track that
// allows two types is filed under the first of them:
//   bit 0 circuit -> 3   bit 1 drag -> 1   bit 2 sprint -> 0   bit 3 drift -> 2
// -1 when the mask has none of the four, which is what free roam reports.
enum {
	PROFILE_HS_SPRINT = 0,
	PROFILE_HS_DRAG = 1,
	PROFILE_HS_DRIFT = 2,
	PROFILE_HS_CIRCUIT = 3,
	PROFILE_HS_NONE = -1,
};
int profile_high_score_race_type(unsigned int raceTypeMask);

// HighScoresDatabase::MaybeAddTrackHighScore (Speed.elf 0x801314FC), the one
// thing that ever writes a record. The key is the three fields retail takes
// straight off TheRaceParameters -- +0x00 the track, +0x04 the direction
// (1 reversed) and +0x20 the lap count -- so a track keeps a separate best for
// each direction and each length.
//
// `bestLapTicks` is FinishedRaceStatsEntry +0x10, in 1/4000 s, and is what
// every type but drift stores; `driftScore` is +0x140 and is what drift stores
// instead. A record is written when there is none for the key yet (and the
// value is worth keeping: a non-drift time must be non-zero, not 0x7FFFFFFF
// and not round to 0.000 s; a drift score must not be negative), or when
// DidPlayerBreakRecord (0x80131464) says this run beat the stored one --
// quicker for a time, higher for a drift score.
//
// Returns 1 when a record was written, which is what retail puts in
// FinishedRaceStatsEntry +0x220 for the post-race screens to flag a new
// record, and 0 when nothing changed (including a full table: retail's 320
// records are all there is, and GetEmptyHighScore returning none is a quiet
// no-op).
//
// PORT: the rest of CommitHighScoresPostRace -- HighScoresDatabase +0xA00 to
// +0xA50, the career totals (races entered and won per type, winnings, the
// drag counter at +0xA4C) -- is not here. Nothing in the port reads them yet;
// they belong with UI_Status_Career (TASKS.md #259 item 7).
int profile_commit_high_score(Profile* p, int raceType, int trackId, int kind,
	int category, int bestLapTicks, int driftScore);
