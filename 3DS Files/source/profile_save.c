// See profile_save.h for the format citation and scope, and
// docs/formats/savefile.md Part 2 for the full derivation. Implementation
// notes specific to this file are inline below.
#include "profile_save.h"
#include "dbglog.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <3ds.h>

// The same debug log main.c uses (sdmc:/nfsu2_debug.txt), through dbglog.c.
static void pslog(const char* msg) {
	dbglog_line(NULL, msg);
}

static void pslogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void pslogf(const char* fmt, ...) {
	char b[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	pslog(b);
}

// ---------------------------------------------------------------------------
// block-B field offsets (docs/formats/savefile.md Part 2, "Block B -- full
// field map"). Named constants rather than inline literals so every read
// below is traceable back to one table row.
// ---------------------------------------------------------------------------
#define OFF_STOCKCARS      0x000000 // FEStockCar[48],  stride 0x18
#define OFF_NUM_STOCKCARS  0x000480
#define OFF_TUNEDCARS      0x000484 // FECarConfig[20], stride 0x430
#define OFF_CAREERCARS     0x005844 // FECareerCar[5],  stride 0x7F2
#define OFF_NUM_CAREERCARS 0x007FFE
#define OFF_ONLINECARS     0x008002 // FECarConfig[6],  stride 0x434
#define OFF_NUM_ONLINECARS 0x00993A
#define OFF_STAGEINFO      0x00993E // RuntimeCareerStageInfo[8], stride 0x101
#define STRIDE_STAGEINFO   0x101
#define STAGE_WORLD_RACES  0x000    // u32[35]
#define STAGE_SPONSOR_RACES 0x08C   // u32[4]
#define STAGE_URL_RACES    0x09C    // u32[10]
#define STAGE_SHOWCASES    0x0C4    // u32[6], showcase handles
#define STAGE_OUTRUN_WINS  0x0DC    // s32
#define STAGE_SPONSOR_PTS  0x0E0    // f32
#define STAGE_SPONSOR_PTS_RACES 0x0E4 // s32
#define STAGE_SPONSORS     0x0E8    // u32[5], name hashes
#define STAGE_COMPLETED    0x0FC    // u8, 1 = completed
#define OFF_CAREERBODY     0x00A146 // PlayerCareerState body (after the 8 stage-info records)
#define OFF_SMSQUEUE       0x00AE76 // SMSMessageQueue, 0x1409 (FUN_005014f0)
#define OFF_CAREERTAIL     0x00C27F // the world map's 16 filter bytes come first
#define CAREERTAIL_FILTERS 16
// CareerSpeech's 0x190 bytes follow the filters (PlayerCareerState +0x887C,
// SaveToBuffer SPEED2.EXE 0x514530); the active career event's handle comes
// after it, at 0xC41F.
#define OFF_CAREERSPEECH   (OFF_CAREERTAIL + CAREERTAIL_FILTERS)
// The EA Trax song table, UserProfile +0x2D8, 27 x {u32 song, u8 play, pad}
// (options.h), saved whole just before the options.
#define OFF_TRAX_SONGS     0x00C423
// OptionsSettings, saved block by block in this order by
// UserProfile::SaveToBuffer: AudioSettings 0x3C, GameplaySettings 0x10,
// PlayerSettings[2] 0x50, VideoSettings 0x10, each a raw copy of the
// in-memory block (options.h).
#define OFF_OPT_AUDIO      0x00C4FB
#define OFF_OPT_GAMEPLAY   0x00C537
#define OFF_OPT_PLAYER     0x00C547
#define OFF_OPT_VIDEO      0x00C597
#define OFF_CAREERSETTINGS 0x00C723
// HighScoresDatabase, 0xA50 bytes; the first 320 x 8 are the records
// profile_best_time()/profile_best_score() scan (profile_save.h).
#define OFF_HIGHSCORES     0x00C731
// The two bytes of the pause menu's Autosave option, UserProfile +0xA0F9 and
// +0xA0F8 on PC (the GameCube's +0x8894 and +0x8890). GOAutoSave's handler
// (SPEED2.EXE 0x49B170; SetToAutosave/SetToPromptSave/SetToNoSave, Speed.elf
// 0x800EC28C..) cycles them through Auto 1/1, Prompt 1/0 and Off 0/0.
#define OFF_AUTOSAVE_NOPROMPT 0x00C72B
#define OFF_AUTOSAVE          0x00C72C
#define OFF_PROFILENAME    0x00D201 // char[32]

// SMSMessageQueue: 256 slots of 0x14, then u32 queued, u32 unread, u8. The
// four flag bytes were read out of the GameCube build, each from the one
// function that writes it:
//   +0x04 read      NotifyReadMessage   0x8012c588 stores 1, and decrements
//                                       the cached unread count at +0x1404
//   +0x05 popped    MarkAllAutoPopupsSeen 0x8012bcc4 stores 1 for the
//                                       messages whose data record says
//                                       auto-popup (AnyUnseenAutoPopups)
//   +0x06 deleted   NotifyDeleteMessage 0x8012c610 stores 1 (and sets +0x04)
//   +0x07 sortable  MarkAllReadMessagesSortable 0x8012c3f4 stores 1 once read
// GetNumMessagesUnread (0x8012c36c) counts a slot when +0x04 == 0 and
// +0x06 != 1, which is the test below. +0x00 is a message name hash on disk,
// resolved through GetSMSMessageDataByNameHash at load; 0 marks an empty slot.
#define SMS_SLOTS       256
#define SMS_SLOT_STRIDE 0x14
#define SMS_SLOT_ID     0x00
#define SMS_SLOT_READ   0x04
#define SMS_SLOT_POPPED  0x05
#define SMS_SLOT_DELETED 0x06
#define SMS_SLOT_SORTABLE 0x07
#define SMS_SLOT_BODYFRAME 0x08
#define SMS_SLOT_ARG      0x0C
#define SMS_SLOT_SUBJFRAME 0x10
// The queue's own fields, after the 256 slots: the number queued, the number
// unread, then a byte. SaveToBuffer writes a word for the third too; the PC
// build's block is 0x1409 bytes, so there it is the one byte.
#define SMS_QUEUE_COUNT  0x1400
#define SMS_QUEUE_UNREAD 0x1404

#define STRIDE_STOCK  0x18
#define STRIDE_TUNED  0x430
#define STRIDE_CAREER 0x7F2
#define STRIDE_ONLINE 0x434

// FECarRecord/FEStockCar/FECarConfig field offsets within one record.
#define REC_NAMEHASH    0x04
#define REC_PERF        0x08
#define REC_INUSE       0x0C
#define REC_FILTER      0x10
#define REC_CARTYPE     0x14 // FEStockCar only: index into the car-data table
#define REC_CARTYPEHASH 0x1C // FECarConfig/FECareerCar: EA hash of the car name
#define REC_CAREER_LOCKED 0x430 // FECareerCar only, u8 (ProfileCarRecord.locked)
#define REC_CAREER_CHOSEN 0x431 // FECareerCar only, u8
// FECarConfig/FECareerCar: PresetCarSlot +0x00, set by FillWithRide.
#define REC_HASRIDE     0x14
// The rest of an FECareerCar that FillFromStockCar and CareerCarTrade reset
// (profile_save.h, PROFILE_CAREER_WRITE_*): PresetCarSlot +0x2B4, the
// PhysicsUpgradeSpecification FillWithRide copies out of the RideInfo
// (0x25 words); +0x348, the five 0x29-byte CustomPhysicsTunings
// DefaultCustomTuningSpecs resets to 100 each; and the OwnedPartInventory
// after the two flag bytes. Every one of them is visible in the user's save:
// the open slot carries 0x94 zero bytes, 0xCD bytes of 0x64 and 0x3C0 zero
// bytes at exactly these offsets.
#define REC_CAREER_PERF_SPEC   0x2C8
#define REC_CAREER_PERF_SPEC_N 0x94
#define REC_CAREER_TUNING      0x35C
#define REC_CAREER_TUNING_N    0xCD
#define REC_CAREER_TUNING_DEF  0x64
_Static_assert(REC_CAREER_TUNING_N == PROFILE_NUM_TUNING_SETTINGS * PROFILE_TUNING_SIZE &&
               REC_CAREER_TUNING_DEF == PROFILE_TUNING_DEFAULT,
               "the record's tuning blocks are Profile's careerTuning / tunedTuning");
#define REC_CAREER_INVENTORY   0x432
#define REC_CAREER_INVENTORY_N 0x3C0

// Inside those two blocks, the performance state Profile keeps (see
// profile_save.h). The inventory offsets are OwnedPartInventory::SaveToBuffer's
// own write order (Speed.elf 0x80135AE8), which is not the object's order: the
// brand list at object +0x2AC is written at +0x2A0 and the spend at object
// +0x3BC at +0x294.
#define INV_OWNED_BITS  0x15C // 16 bytes
#define INV_SPEND       0x294 // u32
#define INV_BRANDS      0x2A0 // 68 u32
// PhysicsUpgradeSpecification: 20 path floats, then one byte an
// ePhysicsPerformanceParts (IsPerfPartInstalled, 0x801C4270).
#define SPEC_UPGRADES   0x00
#define SPEC_INSTALLED  0x50

// ---------------------------------------------------------------------------
// PresetCarSlot: the customisation array a tuned/career/online record carries.
//
// It starts at REC_CARTYPEHASH and is indexed directly by CarSlotId, so the
// car-name hash that field already reads *is* slot 0. Everything a player
// changes about a car -- every body part, every paint, every vinyl layer and
// colour, every decal -- is one u32 in here, and each u32 is the bStringHash of
// a part name rather than a value.
//
// CarSlotId itself is not guesswork: SPEED2.EXE .rdata 0x402e1c is a table of
// { const char* name, u32 id } with 163 entries running from WIDE_BODY = 7 to
// WHEEL_MANUFACTURER = 0xa9, and a second table beside it supplies ids 0..6.
// The ids below are read straight off it. The alignment was then checked
// against a real save from four directions that cannot all be coincidence:
// FRONT_BRAKE/REAR_BRAKE hold one identical hash and FRONT_WHEEL/REAR_WHEEL
// another (brakes and wheels are bought in pairs); the sixteen
// VINYL_COLOUR<layer>_<n> slots hold exactly sixteen VINYL_L*_COLOR* parts;
// the four *_CARBON slots hold four copies of one carbon part; and BASE_PAINT
// holds a GLOSS_* part, which is what a body colour is called.
#define REC_SLOTARRAY   REC_CARTYPEHASH

#define PROFILE_SLOT_BASE_PAINT    0x40
#define PROFILE_SLOT_VINYL_LAYER0  0x41 // ..0x44, one per layer
#define PROFILE_SLOT_PAINT_SPOILER 0x46
#define PROFILE_SLOT_PAINT_RIM     0x4A
#define PROFILE_SLOT_PAINT_SPINNER 0x4B
#define PROFILE_SLOT_PAINT_ROOF    0x4C
#define PROFILE_SLOT_PAINT_MIRROR  0x4D

// Neon, past the 56 DECAL_<surface>_TEX<n> slots and WINDOW_TINT. Read off the
// CarSlotId enum table in SPEED2.EXE's .rdata (file offset 0x402e1c, 8-byte
// { const char*, u32 } pairs). Take the *first* of the two adjacent tables
// there: it is indices 0..162 and is this enum, while the second reuses the
// low ids for a different one.
#define PROFILE_SLOT_NEON          0x9B
#define PROFILE_SLOT_NEON_ENGINE   0x9C
#define PROFILE_SLOT_NEON_CABIN    0x9D
#define PROFILE_SLOT_NEON_TRUNK    0x9E
#define PROFILE_SLOT_CABIN_NEON_FRAME 0x9F

// The highest slot any of the above reads, used to check a record is long
// enough to have one at all -- FEStockCar's 0x18 bytes are not.
#define PROFILE_SLOT_MAX           PROFILE_SLOT_CABIN_NEON_FRAME

// PlayerCareerState body, relative to OFF_CAREERBODY.
#define B_CASH            0x000 // u32 CareerWallet balance
#define B_JUMP_TO_GARAGE  0x004 // u8, see Profile.jumpToGarage
#define B_RACES_ENTERED   0x005 // u32, see Profile.racesEntered
#define B_STAGE0_TIMEOUT  0x009 // u8, see Profile.stage0Timeout
#define B_CS_SEE_EVENTS   0x00A // u8, see Profile.csCanSeeEvents
#define B_CAREER_STAGE    0x00B // u32
#define B_NUM_RECORDS     0x00F // u32
#define B_STATE_RECORDS   0x013 // StateRecord[384], 8 bytes each
#define B_CAREER_STARTED  0xC13 // u8, see Profile.careerStarted
#define B_HAS_CAREER_CAR  0xC14 // u8, see ProfileData.hasCareerCar
#define B_CURRENT_CAR     0xC15 // u32 career-car record hash
#define B_RESPAWN_VALID   0xC19 // u8, see Profile.respawnValid
#define B_RESPAWN_POS     0xC1A // bVector3 + pad
#define B_RESPAWN_HEADING 0xC2A // u16
#define B_CAREER_TIME     0xC2C // float, see Profile.careerTime
#define B_BANK_FLAGS      0xC30 // u8[256]

// ---------------------------------------------------------------------------
// MC02 checksum. FUN_006fcd1c -> FUN_006fe173 @ 0x6fe173. The table at
// 0x8145e0 is the standard MSB-first CRC-32 table for poly 0x04C11DB7, so it
// is generated here rather than embedded -- but note the unusual
// first-four-bytes-preload formulation: a stock CRC-32 will NOT reproduce
// these values.
// ---------------------------------------------------------------------------
static unsigned int s_crcTable[256];
static bool s_crcTableReady = false;

static void crcInit(void) {
	if (s_crcTableReady) return;
	for (int i = 0; i < 256; i++) {
		unsigned int c = (unsigned int)i << 24;
		for (int b = 0; b < 8; b++)
			c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
		s_crcTable[i] = c;
	}
	s_crcTableReady = true;
}

static unsigned int mcCrc(const unsigned char* p, size_t len) {
	if (len < 4) return 0;
	crcInit();
	unsigned int v = ~(((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	                   ((unsigned int)p[2] << 8) | (unsigned int)p[3]);
	for (size_t i = 4; i < len; i++)
		v = ((v << 8) | p[i]) ^ s_crcTable[v >> 24];
	return ~v;
}

// ---------------------------------------------------------------------------
// little-endian scalar reads. The payload is little-endian on PC and the 3DS
// is little-endian too, but these go byte-by-byte anyway: nothing in the
// payload is guaranteed aligned (the profile name sits at 0xD201, an odd
// offset), and an unaligned u32 load faults on ARM11.
// ---------------------------------------------------------------------------
static unsigned int rdU32(const unsigned char* b, size_t o) {
	return (unsigned int)b[o] | ((unsigned int)b[o + 1] << 8) |
	       ((unsigned int)b[o + 2] << 16) | ((unsigned int)b[o + 3] << 24);
}

static unsigned short rdU16(const unsigned char* b, size_t o) {
	return (unsigned short)((unsigned int)b[o] | ((unsigned int)b[o + 1] << 8));
}

static float rdF32(const unsigned char* b, size_t o) {
	unsigned int v = rdU32(b, o);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

static void wrU32(unsigned char* b, size_t o, unsigned int v) {
	b[o]     = (unsigned char)(v & 0xFF);
	b[o + 1] = (unsigned char)((v >> 8) & 0xFF);
	b[o + 2] = (unsigned char)((v >> 16) & 0xFF);
	b[o + 3] = (unsigned char)((v >> 24) & 0xFF);
}

unsigned int profile_ea_hash(const char* s) {
	unsigned int v = 0xFFFFFFFFu;
	for (const unsigned char* p = (const unsigned char*)s; *p; p++)
		v = v * 33u + *p;
	return v;
}

// ---------------------------------------------------------------------------
// save root
// ---------------------------------------------------------------------------
#define LEGACY_PROFILE_DIR "sdmc:/nfsu2data/profiles"

static char s_saveRoot[PROFILE_PATH_MAX] = "sdmc:/nfsu2data/SAVE/NFS Underground 2";

void profile_set_save_root(const char* sdmcSaveRoot) {
	if (!sdmcSaveRoot || !sdmcSaveRoot[0]) return;
	strncpy(s_saveRoot, sdmcSaveRoot, sizeof(s_saveRoot) - 1);
	s_saveRoot[sizeof(s_saveRoot) - 1] = '\0';
	// Trim a trailing slash so path joins below stay single-slashed.
	size_t n = strlen(s_saveRoot);
	while (n > 0 && (s_saveRoot[n - 1] == '/' || s_saveRoot[n - 1] == '\\'))
		s_saveRoot[--n] = '\0';
	pslogf("profile: save root = %s", s_saveRoot);
}

const char* profile_save_root(void) { return s_saveRoot; }

// Creates every component of the save root, then the per-profile directory.
// The sdmc devoptab has no mkdir -p, and EEXIST is not an error here.
static void mkdirChain(const char* path) {
	char tmp[PROFILE_PATH_MAX];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	// Skip past the "sdmc:" drive prefix before looking for separators.
	char* p = strchr(tmp, ':');
	p = p ? p + 1 : tmp;
	if (*p == '/') p++;
	for (; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		mkdir(tmp, 0777);
		*p = '/';
	}
	mkdir(tmp, 0777);
}

// ---------------------------------------------------------------------------
// container
// ---------------------------------------------------------------------------

// Validates an MC02 file exactly as FUN_006fd2f6 does and returns the
// payload. `data`/`len` is the whole file. Returns NULL (having logged the
// specific reason) if the real game would have rejected it -- error 8
// "corrupt" in its own terms. `wantB` is the payload size the caller's load
// asks for (the MemoryCard op's own size), `kind` what it is called in the log.
static const unsigned char* mc02PayloadSized(const unsigned char* data, size_t len,
		const char* what, unsigned int wantB, const char* kind) {
	if (len < PROFILE_MC02_DATA_OFF) {
		pslogf("profile: %s is %u bytes -- too short for an MC02 header", what, (unsigned)len);
		return NULL;
	}
	unsigned int magic = rdU32(data, 0x00);
	unsigned int total = rdU32(data, 0x04);
	unsigned int sizeA = rdU32(data, 0x08);
	unsigned int sizeB = rdU32(data, 0x0C);
	unsigned int sumA  = rdU32(data, 0x10);
	unsigned int sumB  = rdU32(data, 0x14);
	unsigned int sumH  = rdU32(data, 0x18);

	if (magic != PROFILE_MC02_MAGIC) {
		pslogf("profile: %s bad magic 0x%08X (want 'MC02')", what, magic);
		return NULL;
	}
	if (total != (unsigned int)len) {
		pslogf("profile: %s header totalSize %u != file length %u", what, total, (unsigned)len);
		return NULL;
	}
	if ((size_t)PROFILE_MC02_HDR_SIZE + sizeA + sizeB > len) {
		pslogf("profile: %s block sizes (%u + %u) overrun the file", what, sizeA, sizeB);
		return NULL;
	}

	unsigned int gotH = mcCrc(data, 0x18);
	unsigned int gotA = mcCrc(data + PROFILE_MC02_HDR_SIZE, sizeA);
	unsigned int gotB = mcCrc(data + PROFILE_MC02_HDR_SIZE + sizeA, sizeB);
	if (gotH != sumH || gotA != sumA || gotB != sumB) {
		pslogf("profile: %s checksum mismatch hdr %08X/%08X A %08X/%08X B %08X/%08X",
			what, sumH, gotH, sumA, gotA, sumB, gotB);
		return NULL;
	}
	if (sizeB != wantB) {
		pslogf("profile: %s payload is %u bytes, expected %u -- not %s",
			what, sizeB, wantB, kind);
		return NULL;
	}
	return data + PROFILE_MC02_HDR_SIZE + sizeA;
}

static const unsigned char* mc02Payload(const unsigned char* data, size_t len, const char* what) {
	return mc02PayloadSized(data, len, what, PROFILE_PAYLOAD_SIZE, "a UserProfile save");
}

// ---------------------------------------------------------------------------
// block-B parsing
// ---------------------------------------------------------------------------
static unsigned int rdSlot(const unsigned char* b, size_t rec, unsigned int slot) {
	return rdU32(b, rec + REC_SLOTARRAY + (size_t)slot * 4);
}

static void readRecords(const unsigned char* b, size_t base, size_t stride, int count,
                        ProfileCarRecord* out, bool carTypeIsHash) {
	// Only the records that are a whole PresetCarSlot longer than FECarRecord
	// carry the customisation array; FEStockCar's 0x18 bytes stop well short of
	// it, and reading a slot out of one would just be reading the next record.
	const bool hasSlots = carTypeIsHash &&
		stride >= REC_SLOTARRAY + (PROFILE_SLOT_MAX + 1) * 4;
	for (int i = 0; i < count; i++) {
		size_t o = base + (size_t)i * stride;
		out[i].nameHash    = rdU32(b, o + REC_NAMEHASH);
		out[i].perf        = rdF32(b, o + REC_PERF);
		out[i].inUse       = rdU32(b, o + REC_INUSE);
		out[i].filter      = rdU32(b, o + REC_FILTER);
		out[i].carType     = carTypeIsHash ? 0 : rdU32(b, o + REC_CARTYPE);
		out[i].carTypeHash = carTypeIsHash ? rdU32(b, o + REC_CARTYPEHASH) : 0;
		if (!hasSlots)
			continue;
		out[i].hasRide      = rdU32(b, o + REC_HASRIDE);
		out[i].basePaint    = rdSlot(b, o, PROFILE_SLOT_BASE_PAINT);
		out[i].paintRoof    = rdSlot(b, o, PROFILE_SLOT_PAINT_ROOF);
		out[i].paintSpoiler = rdSlot(b, o, PROFILE_SLOT_PAINT_SPOILER);
		out[i].paintMirror  = rdSlot(b, o, PROFILE_SLOT_PAINT_MIRROR);
		out[i].paintRim     = rdSlot(b, o, PROFILE_SLOT_PAINT_RIM);
		out[i].paintSpinner = rdSlot(b, o, PROFILE_SLOT_PAINT_SPINNER);
		for (unsigned int v = 0; v < 4; v++)
			out[i].vinylLayer[v] = rdSlot(b, o, PROFILE_SLOT_VINYL_LAYER0 + v);
		out[i].neon           = rdSlot(b, o, PROFILE_SLOT_NEON);
		out[i].neonEngine     = rdSlot(b, o, PROFILE_SLOT_NEON_ENGINE);
		out[i].neonCabin      = rdSlot(b, o, PROFILE_SLOT_NEON_CABIN);
		out[i].neonTrunk      = rdSlot(b, o, PROFILE_SLOT_NEON_TRUNK);
		out[i].cabinNeonFrame = rdSlot(b, o, PROFILE_SLOT_CABIN_NEON_FRAME);
	}
}

// The four settings blocks are raw little-endian copies of the PC structs, the
// same byte order and float format as the 3DS, so they are copied whole.
static void writeOptions(unsigned char* b, const ProfileOptions* o) {
	memcpy(b + OFF_OPT_AUDIO, &o->audio, sizeof(o->audio));
	memcpy(b + OFF_OPT_GAMEPLAY, &o->gameplay, sizeof(o->gameplay));
	memcpy(b + OFF_OPT_PLAYER, o->player, sizeof(o->player));
	memcpy(b + OFF_OPT_VIDEO, &o->video, sizeof(o->video));
	memcpy(b + OFF_TRAX_SONGS, o->trax, sizeof(o->trax));
}

static void readOptions(const unsigned char* b, ProfileOptions* o) {
	_Static_assert(OFF_OPT_AUDIO + sizeof(OptAudio) == OFF_OPT_GAMEPLAY &&
	               OFF_OPT_GAMEPLAY + sizeof(OptGameplay) == OFF_OPT_PLAYER &&
	               OFF_OPT_PLAYER + sizeof(OptPlayer) * OPTIONS_PLAYERS == OFF_OPT_VIDEO,
	               "the settings blocks are contiguous in the payload");
	_Static_assert(OFF_TRAX_SONGS + sizeof(OptTraxSong) * OPTIONS_TRAX_SONGS == OFF_OPT_AUDIO,
	               "the EA Trax table ends where AudioSettings starts");
	memcpy(&o->audio, b + OFF_OPT_AUDIO, sizeof(o->audio));
	memcpy(&o->gameplay, b + OFF_OPT_GAMEPLAY, sizeof(o->gameplay));
	memcpy(o->player, b + OFF_OPT_PLAYER, sizeof(o->player));
	memcpy(&o->video, b + OFF_OPT_VIDEO, sizeof(o->video));

	// PORT: saves created by this port before the options were decoded carry
	// all four blocks as zeros -- every volume at 0, no camera. Retail can
	// never write that (Default puts Stereo, Far and a dozen 1s in them and no
	// row can clear those), so a block range that is zero end to end is one of
	// those saves and gets the shipped defaults instead of silence.
	bool allZero = true;
	for (size_t i = OFF_OPT_AUDIO; i < OFF_OPT_VIDEO + sizeof(OptVideo); i++) {
		if (b[i]) {
			allZero = false;
			break;
		}
	}
	if (allZero) {
		options_default(o, 0);
		pslog("profile options: all four blocks are zero (an early port save) -- using the defaults");
	}

	// The same for the EA Trax table, which this port's saves carried as zeros
	// until it was decoded -- and any table that is not a permutation of the
	// 27 songs, which retail cannot write either (options_trax_valid).
	memcpy(o->trax, b + OFF_TRAX_SONGS, sizeof(o->trax));
	if (!options_trax_valid(o->trax)) {
		options_default_trax(o->trax);
		pslog("profile options: the EA Trax table is not a valid one -- using the default table");
	}
}

unsigned int profile_career_slot_hash(int slot) {
	char name[24];
	snprintf(name, sizeof(name), "CAREER_SLOT_%d", slot);
	return profile_ea_hash(name);
}

void profile_career_choose(Profile* p, int slot, const unsigned int parts[PROFILE_CAR_SLOTS],
	float perf, bool traded)
{
	if (!p || slot < 0 || slot >= PROFILE_NUM_CAREER_SLOTS || !parts)
		return;
	ProfileCarRecord* c = &p->careerCar[slot];
	memcpy(p->careerParts[slot], parts, sizeof(p->careerParts[slot]));
	c->perf = perf;
	c->inUse = 1;
	c->filter = PROFILE_FILTER_CAREER;
	c->chosen = 1;
	// FillWithRide's PresetCarSlot +0x00 and its slot 0, which is the field
	// the record's car is joined to the car table by.
	c->hasRide = 1;
	c->carTypeHash = parts[0];
	// What readRecords() keeps out of the same array.
	c->basePaint    = parts[PROFILE_SLOT_BASE_PAINT];
	c->paintRoof    = parts[PROFILE_SLOT_PAINT_ROOF];
	c->paintSpoiler = parts[PROFILE_SLOT_PAINT_SPOILER];
	c->paintMirror  = parts[PROFILE_SLOT_PAINT_MIRROR];
	c->paintRim     = parts[PROFILE_SLOT_PAINT_RIM];
	c->paintSpinner = parts[PROFILE_SLOT_PAINT_SPINNER];
	for (unsigned int v = 0; v < 4; v++)
		c->vinylLayer[v] = parts[PROFILE_SLOT_VINYL_LAYER0 + v];
	c->neon           = parts[PROFILE_SLOT_NEON];
	c->neonEngine     = parts[PROFILE_SLOT_NEON_ENGINE];
	c->neonCabin      = parts[PROFILE_SLOT_NEON_CABIN];
	c->neonTrunk      = parts[PROFILE_SLOT_NEON_TRUNK];
	c->cabinNeonFrame = parts[PROFILE_SLOT_CABIN_NEON_FRAME];
	// FillFromStockCar's DefaultCustomTuningSpecs (0x80135004).
	memset(p->careerTuning[slot], PROFILE_TUNING_DEFAULT, sizeof(p->careerTuning[slot]));
	p->careerCarWrite[slot] |= PROFILE_CAREER_WRITE_FILLED |
		(traded ? PROFILE_CAREER_WRITE_TRADED : 0);
}

// What readRecords() keeps of a record's slot array, refreshed from `parts`.
static void recordFromParts(ProfileCarRecord* c, const unsigned int* parts) {
	c->carTypeHash  = parts[0];
	c->basePaint    = parts[PROFILE_SLOT_BASE_PAINT];
	c->paintRoof    = parts[PROFILE_SLOT_PAINT_ROOF];
	c->paintSpoiler = parts[PROFILE_SLOT_PAINT_SPOILER];
	c->paintMirror  = parts[PROFILE_SLOT_PAINT_MIRROR];
	c->paintRim     = parts[PROFILE_SLOT_PAINT_RIM];
	c->paintSpinner = parts[PROFILE_SLOT_PAINT_SPINNER];
	for (unsigned int v = 0; v < 4; v++)
		c->vinylLayer[v] = parts[PROFILE_SLOT_VINYL_LAYER0 + v];
	c->neon           = parts[PROFILE_SLOT_NEON];
	c->neonEngine     = parts[PROFILE_SLOT_NEON_ENGINE];
	c->neonCabin      = parts[PROFILE_SLOT_NEON_CABIN];
	c->neonTrunk      = parts[PROFILE_SLOT_NEON_TRUNK];
	c->cabinNeonFrame = parts[PROFILE_SLOT_CABIN_NEON_FRAME];
}

// FindEmptyTunedCarSlot (Speed.elf 0x8013427C): the first record whose name
// hash (+4) is 0.
int profile_tuned_free_slot(const Profile* p) {
	if (!p)
		return -1;
	for (int i = 0; i < PROFILE_NUM_TUNED_SLOTS; i++)
		if (p->tunedCar[i].nameHash == 0)
			return i;
	return -1;
}

// AddNewTunedCar (0x80134314): FillFromStockCar on the empty slot, named
// FEngHashString("TUNED_CAR_%d", index). The record is the stock car's slot
// array with a fresh PhysicsUpgradeSpecification and default tunings.
bool profile_tuned_add(Profile* p, int slot, const unsigned int parts[PROFILE_CAR_SLOTS],
	float perf)
{
	if (!p || slot < 0 || slot >= PROFILE_NUM_TUNED_SLOTS || !parts)
		return false;
	char name[24];
	snprintf(name, sizeof(name), "TUNED_CAR_%d", slot);
	ProfileCarRecord* c = &p->tunedCar[slot];
	memset(c, 0, sizeof(*c));
	c->nameHash = profile_ea_hash(name);
	c->perf = perf;
	c->inUse = 1;
	c->filter = PROFILE_FILTER_TUNED;
	c->hasRide = 1;
	memcpy(p->tunedParts[slot], parts, sizeof(p->tunedParts[slot]));
	recordFromParts(c, p->tunedParts[slot]);
	memset(p->tunedInstalledParts[slot], 0, sizeof(p->tunedInstalledParts[slot]));
	memset(p->tunedUpgradePath[slot], 0, sizeof(p->tunedUpgradePath[slot]));
	memset(p->tunedTuning[slot], PROFILE_TUNING_DEFAULT, sizeof(p->tunedTuning[slot]));
	p->tunedCarWrite[slot] |= PROFILE_CAREER_WRITE_FILLED;
	return true;
}

// DeleteTunedCar: the record back to an empty one.
void profile_tuned_delete(Profile* p, int slot) {
	if (!p || slot < 0 || slot >= PROFILE_NUM_TUNED_SLOTS)
		return;
	memset(&p->tunedCar[slot], 0, sizeof(p->tunedCar[slot]));
	memset(p->tunedParts[slot], 0, sizeof(p->tunedParts[slot]));
	memset(p->tunedInstalledParts[slot], 0, sizeof(p->tunedInstalledParts[slot]));
	memset(p->tunedUpgradePath[slot], 0, sizeof(p->tunedUpgradePath[slot]));
	// The empty record's tunings are DefaultCustomTuningSpecs' 100s, as the
	// FILLED reset writes them and every unused tuned record in a save holds.
	memset(p->tunedTuning[slot], PROFILE_TUNING_DEFAULT, sizeof(p->tunedTuning[slot]));
	p->tunedCarWrite[slot] |= PROFILE_CAREER_WRITE_FILLED;
}

void profile_tuned_to_work(Profile* p, int slot) {
	if (!p || slot < 0 || slot >= PROFILE_NUM_TUNED_SLOTS)
		return;
	const int w = PROFILE_WORK_SLOT;
	p->careerCar[w] = p->tunedCar[slot];
	p->careerCar[w].locked = 0;
	p->careerCar[w].chosen = 1;
	memcpy(p->careerParts[w], p->tunedParts[slot], sizeof(p->careerParts[w]));
	memcpy(p->careerInstalledParts[w], p->tunedInstalledParts[slot],
		sizeof(p->careerInstalledParts[w]));
	memcpy(p->careerUpgradePath[w], p->tunedUpgradePath[slot], sizeof(p->careerUpgradePath[w]));
	memcpy(p->careerTuning[w], p->tunedTuning[slot], sizeof(p->careerTuning[w]));
	memset(p->careerOwnedParts[w], 0, sizeof(p->careerOwnedParts[w]));
	memset(p->careerOwnedBrands[w], 0, sizeof(p->careerOwnedBrands[w]));
	memset(p->careerVisualInv[w], 0, sizeof(p->careerVisualInv[w]));
	p->careerPartsSpend[w] = 0;
	p->careerCarWrite[w] = 0;
}

void profile_preset_to_work(Profile* p, const unsigned int parts[PROFILE_CAR_SLOTS]) {
	if (!p || !parts)
		return;
	const int w = PROFILE_WORK_SLOT;
	memset(&p->careerCar[w], 0, sizeof(p->careerCar[w]));
	recordFromParts(&p->careerCar[w], parts);
	p->careerCar[w].chosen = 1;
	memcpy(p->careerParts[w], parts, sizeof(p->careerParts[w]));
	memset(p->careerInstalledParts[w], 0, sizeof(p->careerInstalledParts[w]));
	memset(p->careerUpgradePath[w], 0, sizeof(p->careerUpgradePath[w]));
	memset(p->careerTuning[w], 0, sizeof(p->careerTuning[w]));
	memset(p->careerOwnedParts[w], 0, sizeof(p->careerOwnedParts[w]));
	memset(p->careerOwnedBrands[w], 0, sizeof(p->careerOwnedBrands[w]));
	memset(p->careerVisualInv[w], 0, sizeof(p->careerVisualInv[w]));
	p->careerPartsSpend[w] = 0;
	p->careerCarWrite[w] = 0;
}

void profile_work_to_tuned(Profile* p, int slot) {
	if (!p || slot < 0 || slot >= PROFILE_NUM_TUNED_SLOTS)
		return;
	const int w = PROFILE_WORK_SLOT;
	memcpy(p->tunedParts[slot], p->careerParts[w], sizeof(p->tunedParts[slot]));
	memcpy(p->tunedInstalledParts[slot], p->careerInstalledParts[w],
		sizeof(p->tunedInstalledParts[slot]));
	memcpy(p->tunedUpgradePath[slot], p->careerUpgradePath[w], sizeof(p->tunedUpgradePath[slot]));
	memcpy(p->tunedTuning[slot], p->careerTuning[w], sizeof(p->tunedTuning[slot]));
	p->tunedCar[slot].perf = p->careerCar[w].perf;
	recordFromParts(&p->tunedCar[slot], p->tunedParts[slot]);
}

// Default__17PlayerCareerState (Speed.elf 0x80123934)'s share of the career
// cars, applied to a Profile: DefaultCareerCars (0x801340A4) makes five
// records named CAREER_SLOT_<n>, filter 4 and in use, every one locked and
// none chosen, with OwnedPartInventory::Default on each; then
// CommitUnlockCareerCarSlot's UnlockACareerCarSlot (0x80134048) opens the
// first locked one, slot 0. The current car is FEHashUpper("CAREER_0") (the
// string at 0x8032934C), a name no slot carries, and hasCareerCar is clear --
// so a fresh career has no car, one open slot, and the car lot's buy to fill
// it.
static void defaultCareerCars(Profile* p) {
	for (int i = 0; i < PROFILE_NUM_CAREER_SLOTS; i++) {
		ProfileCarRecord* c = &p->careerCar[i];
		memset(c, 0, sizeof(*c));
		memset(p->careerParts[i], 0, sizeof(p->careerParts[i]));
		memset(p->careerOwnedParts[i], 0, sizeof(p->careerOwnedParts[i]));
		memset(p->careerInstalledParts[i], 0, sizeof(p->careerInstalledParts[i]));
		memset(p->careerOwnedBrands[i], 0, sizeof(p->careerOwnedBrands[i]));
		memset(p->careerUpgradePath[i], 0, sizeof(p->careerUpgradePath[i]));
		memset(p->careerTuning[i], PROFILE_TUNING_DEFAULT, sizeof(p->careerTuning[i]));
		p->careerPartsSpend[i] = 0;
		memset(p->careerVisualInv[i], 0, sizeof(p->careerVisualInv[i]));
		c->nameHash = profile_career_slot_hash(i);
		c->inUse = 1;
		c->filter = PROFILE_FILTER_CAREER;
		c->locked = i != 0;
		c->chosen = 0;
		// The record's tuning bytes as a retail save carries them on a slot that
		// has never held a car, and the inventory Default leaves.
		p->careerCarWrite[i] = PROFILE_CAREER_WRITE_FILLED | PROFILE_CAREER_WRITE_TRADED;
	}
	p->numCareerCars = PROFILE_NUM_CAREER_SLOTS;
	p->currentCarHash = profile_ea_hash("CAREER_0");
	p->hasCareerCar = false;
}

static void parsePayload(const unsigned char* b, Profile* p) {
	memset(p, 0, sizeof(*p));

	memcpy(p->name, b + OFF_PROFILENAME, 32);
	p->name[31] = '\0';

	readRecords(b, OFF_STOCKCARS,  STRIDE_STOCK,  PROFILE_NUM_STOCK_SLOTS,  p->stockCar,  false);
	readRecords(b, OFF_TUNEDCARS,  STRIDE_TUNED,  PROFILE_NUM_TUNED_SLOTS,  p->tunedCar,  true);
	readRecords(b, OFF_CAREERCARS, STRIDE_CAREER, PROFILE_NUM_CAREER_SLOTS, p->careerCar, true);
	readRecords(b, OFF_ONLINECARS, STRIDE_ONLINE, PROFILE_NUM_ONLINE_SLOTS, p->onlineCar, true);
	_Static_assert(STRIDE_CAREER >= REC_SLOTARRAY + PROFILE_CAR_SLOTS * 4 &&
	               PROFILE_SLOT_MAX < PROFILE_CAR_SLOTS,
	               "careerParts must lie inside a career record");
	for (int i = 0; i < PROFILE_NUM_CAREER_SLOTS; i++) {
		const size_t rec = OFF_CAREERCARS + (size_t)i * STRIDE_CAREER;
		for (unsigned int s = 0; s < PROFILE_CAR_SLOTS; s++)
			p->careerParts[i][s] = rdSlot(b, rec, s);
		p->careerCar[i].locked = b[rec + REC_CAREER_LOCKED];
		p->careerCar[i].chosen = b[rec + REC_CAREER_CHOSEN];
		// The performance half of the record: the fitted parts and upgrade
		// paths out of the PhysicsUpgradeSpecification, the owned parts,
		// brands and spend out of the OwnedPartInventory.
		const size_t spec = rec + REC_CAREER_PERF_SPEC;
		const size_t inv = rec + REC_CAREER_INVENTORY;
		for (int u = 0; u < PROFILE_NUM_UPGRADE_PATHS; u++) {
			const unsigned int w = rdU32(b, spec + SPEC_UPGRADES + (size_t)u * 4);
			memcpy(&p->careerUpgradePath[i][u], &w, 4);
		}
		memcpy(p->careerInstalledParts[i], b + spec + SPEC_INSTALLED,
			PROFILE_NUM_PERF_PARTS);
		memcpy(p->careerTuning[i], b + rec + REC_CAREER_TUNING, REC_CAREER_TUNING_N);
		memcpy(p->careerOwnedParts[i], b + inv + INV_OWNED_BITS,
			sizeof(p->careerOwnedParts[i]));
		p->careerPartsSpend[i] = rdU32(b, inv + INV_SPEND);
		for (int k = 0; k < PROFILE_NUM_PERF_PARTS; k++)
			p->careerOwnedBrands[i][k] = rdU32(b, inv + INV_BRANDS + (size_t)k * 4);
		memcpy(p->careerVisualInv[i], b + inv, PROFILE_INV_BLOCK_SIZE);
	}
	// The tuned cars' slot arrays and performance specs (#379), at the same
	// record offsets as a career car's.
	_Static_assert(STRIDE_TUNED >= REC_CAREER_TUNING + REC_CAREER_TUNING_N,
	               "a tuned record holds the slot array, the spec and the tunings");
	for (int i = 0; i < PROFILE_NUM_TUNED_SLOTS; i++) {
		const size_t rec = OFF_TUNEDCARS + (size_t)i * STRIDE_TUNED;
		for (unsigned int s = 0; s < PROFILE_CAR_SLOTS; s++)
			p->tunedParts[i][s] = rdSlot(b, rec, s);
		const size_t spec = rec + REC_CAREER_PERF_SPEC;
		for (int u = 0; u < PROFILE_NUM_UPGRADE_PATHS; u++) {
			const unsigned int w = rdU32(b, spec + SPEC_UPGRADES + (size_t)u * 4);
			memcpy(&p->tunedUpgradePath[i][u], &w, 4);
		}
		memcpy(p->tunedInstalledParts[i], b + spec + SPEC_INSTALLED, PROFILE_NUM_PERF_PARTS);
		memcpy(p->tunedTuning[i], b + rec + REC_CAREER_TUNING, REC_CAREER_TUNING_N);
	}
	p->numStockCars  = rdU32(b, OFF_NUM_STOCKCARS);
	p->numCareerCars = rdU32(b, OFF_NUM_CAREERCARS);
	p->numOnlineCars = rdU32(b, OFF_NUM_ONLINECARS);

	const size_t body = OFF_CAREERBODY;
	p->cash            = rdU32(b, body + B_CASH);
	p->jumpToGarage    = b[body + B_JUMP_TO_GARAGE] != 0;
	p->racesEntered    = rdU32(b, body + B_RACES_ENTERED);
	p->stage0Timeout   = b[body + B_STAGE0_TIMEOUT] != 0;
	p->csCanSeeEvents  = b[body + B_CS_SEE_EVENTS] != 0;
	p->careerStage     = rdU32(b, body + B_CAREER_STAGE);
	p->numStateRecords = rdU32(b, body + B_NUM_RECORDS);
	p->careerStarted   = b[body + B_CAREER_STARTED] != 0;
	p->hasCareerCar    = b[body + B_HAS_CAREER_CAR] != 0;
	p->currentCarHash  = rdU32(b, body + B_CURRENT_CAR);
	p->respawnPos[0]   = rdF32(b, body + B_RESPAWN_POS + 0);
	p->respawnPos[1]   = rdF32(b, body + B_RESPAWN_POS + 4);
	p->respawnPos[2]   = rdF32(b, body + B_RESPAWN_POS + 8);
	p->respawnHeading  = rdU16(b, body + B_RESPAWN_HEADING);
	// PORT: saves the port wrote before #366 never stored the flag, and each
	// port path that wrote a respawn is one where retail sets it, so a respawn
	// point other than the origin counts as set too.
	p->respawnValid    = b[body + B_RESPAWN_VALID] != 0 ||
		p->respawnPos[0] != 0.0f || p->respawnPos[1] != 0.0f || p->respawnPos[2] != 0.0f;
	p->careerTime      = rdF32(b, body + B_CAREER_TIME);
	p->careerSettings  = rdU32(b, OFF_CAREERSETTINGS);
	memcpy(p->mapFilter, b + OFF_CAREERTAIL, CAREERTAIL_FILTERS);
	memcpy(p->speechBlock, b + OFF_CAREERSPEECH, PROFILE_SPEECH_BLOCK);
	p->autosave         = b[OFF_AUTOSAVE] != 0;
	p->autosaveNoPrompt = b[OFF_AUTOSAVE_NOPROMPT] != 0;
	readOptions(b, &p->options);

	// StateRecord[384], 8 bytes each: {u32 eventNameHash, u8, u8, u8 completionState, u8}.
	// The count field at +0x7afc is the game's own tally; the independent
	// scan here is what gets logged beside it, because a disagreement is the
	// cheapest possible signal that the record stride is wrong.
	//
	// The records are kept as well as counted. `stateRecCount` is the game's
	// own tally clamped to the array, because that is how far GetStateRecord
	// searches; anything past it is stale file bytes.
	p->stateRecCount = p->numStateRecords > PROFILE_NUM_STATE_RECORDS
		? PROFILE_NUM_STATE_RECORDS : p->numStateRecords;
	for (int i = 0; i < PROFILE_NUM_STATE_RECORDS; i++) {
		const size_t r = body + B_STATE_RECORDS + (size_t)i * 8;
		if (rdU32(b, r) != 0) p->numCompleted++;
		p->stateRec[i].eventId = rdU32(b, r);
		p->stateRec[i].type    = b[r + 4];
		p->stateRec[i].pad     = b[r + 5];
		p->stateRec[i].state   = b[r + 6];
		p->stateRec[i].pad2    = b[r + 7];
	}
	for (int i = 0; i < PROFILE_NUM_BANK_FLAGS; i++) {
		p->bankFlags[i] = b[body + B_BANK_FLAGS + i];
		if (b[body + B_BANK_FLAGS + i]) p->numBankPickups++;
	}
	for (int s = 0; s < PROFILE_NUM_STAGES; s++) {
		const size_t r = OFF_STAGEINFO + (size_t)s * STRIDE_STAGEINFO;
		ProfileStageInfo* st = &p->stage[s];
		p->stageCompleted[s] = b[r + STAGE_COMPLETED] != 0;
		for (int i = 0; i < PROFILE_STAGE_WORLD_RACES; i++)
			st->worldRace[i] = rdU32(b, r + STAGE_WORLD_RACES + (size_t)i * 4);
		for (int i = 0; i < PROFILE_STAGE_SPONSOR_RACES; i++)
			st->sponsorRace[i] = rdU32(b, r + STAGE_SPONSOR_RACES + (size_t)i * 4);
		for (int i = 0; i < PROFILE_STAGE_URL_RACES; i++)
			st->urlRace[i] = rdU32(b, r + STAGE_URL_RACES + (size_t)i * 4);
		for (int i = 0; i < PROFILE_STAGE_SHOWCASES; i++)
			st->showcase[i] = rdU32(b, r + STAGE_SHOWCASES + (size_t)i * 4);
		st->sponsorPoints = rdF32(b, r + STAGE_SPONSOR_PTS);
		st->sponsorPointRaces = (int)rdU32(b, r + STAGE_SPONSOR_PTS_RACES);
		st->outrunWins = (int)rdU32(b, r + STAGE_OUTRUN_WINS);
		for (int i = 0; i < PROFILE_STAGE_SPONSORS; i++)
			st->sponsor[i] = rdU32(b, r + STAGE_SPONSORS + (size_t)i * 4);
	}
	for (int i = 0; i < PROFILE_NUM_HIGHSCORES; i++) {
		const size_t r = OFF_HIGHSCORES + (size_t)i * 8;
		p->highScores[i].trackId  = (short)rdU16(b, r);
		p->highScores[i].category = b[r + 2];
		p->highScores[i].kind     = b[r + 3];
		p->highScores[i].value    = (int)rdU32(b, r + 4);
	}

	// The loader's own compaction pass (savefile.md): the 256 slots are read
	// in order, the empty ones dropped, and the two summary fields recomputed
	// rather than taken from the file -- which is why both counts come out of
	// this loop and not out of the block's trailing u32s.
	for (int i = 0; i < SMS_SLOTS; i++) {
		const size_t s = OFF_SMSQUEUE + (size_t)i * SMS_SLOT_STRIDE;
		const unsigned int id = rdU32(b, s + SMS_SLOT_ID);
		if (id == 0)
			continue;
		if (p->smsCount < PROFILE_MAX_SMS) {
			ProfileSmsSlot* m = &p->sms[p->smsCount];
			m->slot = (unsigned short)i;
			m->id = id;
			m->read = b[s + SMS_SLOT_READ];
			m->popped = b[s + SMS_SLOT_POPPED];
			m->deleted = b[s + SMS_SLOT_DELETED];
			m->sortable = b[s + SMS_SLOT_SORTABLE];
			m->bodyFrameId = rdU32(b, s + SMS_SLOT_BODYFRAME);
			m->argId = rdU32(b, s + SMS_SLOT_ARG);
			m->subjectFrameId = rdU32(b, s + SMS_SLOT_SUBJFRAME);
		}
		p->smsCount++;
		if (b[s + SMS_SLOT_READ] == 0 && b[s + SMS_SLOT_DELETED] != 1)
			p->smsUnread++;
	}
	if (p->smsCount > PROFILE_MAX_SMS)
		p->smsCount = PROFILE_MAX_SMS;

	// PORT: saves this port created before the career cars were decoded carry
	// none -- the count and all five records zero. Retail cannot write that
	// (Default always leaves five, and nothing removes one), so such a save
	// gets the fresh-career records Default would have given it, and keeps
	// them with its next write.
	bool noCareerCars = p->numCareerCars == 0;
	for (int i = 0; i < PROFILE_NUM_CAREER_SLOTS && noCareerCars; i++)
		if (p->careerCar[i].nameHash != 0)
			noCareerCars = false;
	if (noCareerCars && !p->hasCareerCar) {
		defaultCareerCars(p);
		pslog("profile: no career car records (an early port save) -- using "
		      "Default__17PlayerCareerState's five, slot 0 open");
	}

	p->currentCarSlot = -1;
	for (int i = 0; i < PROFILE_NUM_CAREER_SLOTS; i++) {
		if (p->careerCar[i].nameHash == p->currentCarHash && p->currentCarHash != 0) {
			p->currentCarSlot = i;
			break;
		}
	}
}

bool profile_load_file(const char* path, Profile* out) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		pslogf("profile_load_file: fopen(%s) failed", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > (long)(PROFILE_FILE_SIZE * 2)) {
		pslogf("profile_load_file: %s has implausible length %ld", path, len);
		fclose(f);
		return false;
	}
	unsigned char* data = (unsigned char*)malloc((size_t)len);
	if (!data) {
		pslog("profile_load_file: malloc failed");
		fclose(f);
		return false;
	}
	size_t got = fread(data, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) {
		pslogf("profile_load_file: short read on %s (%u/%ld)", path, (unsigned)got, len);
		free(data);
		return false;
	}

	const unsigned char* payload = mc02Payload(data, (size_t)len, path);
	if (!payload) {
		free(data);
		// #235: the save is rewritten in place, its mirror first. A save torn
		// by a power cut mid-write still has a whole mirror beside it.
		char mirror[PROFILE_PATH_MAX + 8];
		snprintf(mirror, sizeof(mirror), "%s" PROFILE_MIRROR_EXT, path);
		if (strstr(path, PROFILE_MIRROR_EXT) || !profile_load_file(mirror, out))
			return false;
		pslogf("profile_load_file: %s failed its checks -- loaded its mirror instead", path);
		strncpy(out->sourcePath, path, sizeof(out->sourcePath) - 1);
		out->sourcePath[sizeof(out->sourcePath) - 1] = '\0';
		return true;
	}

	parsePayload(payload, out);
	out->isLoaded = true;
	strncpy(out->sourcePath, path, sizeof(out->sourcePath) - 1);
	out->sourcePath[sizeof(out->sourcePath) - 1] = '\0';
	free(data);

	char stages[PROFILE_NUM_STAGES + 1];
	for (int s = 0; s < PROFILE_NUM_STAGES; s++)
		stages[s] = out->stageCompleted[s] ? '1' : '0';
	stages[PROFILE_NUM_STAGES] = '\0';
	int highScores = 0;
	for (int i = 0; i < PROFILE_NUM_HIGHSCORES; i++)
		if (out->highScores[i].trackId != 0) highScores++;
	pslogf("profile_load_file: %s -> \"%s\" cash=$%u stage=%u events=%u/%u banks=%u "
	       "cars stock=%u career=%u current=%s sms=%u/%u unread stages=%s highscores=%d",
		path, out->name, out->cash, out->careerStage,
		out->numCompleted, out->numStateRecords, out->numBankPickups,
		out->numStockCars, out->numCareerCars,
		out->currentCarSlot >= 0 ? "yes" : "none",
		out->smsUnread, out->smsCount, stages, highScores);
	return true;
}

// ---------------------------------------------------------------------------
// enumeration
// ---------------------------------------------------------------------------

// Legacy pre-C1 profiles this port wrote: a bare 48,018-byte GameCube-layout
// buffer with no container. Kept readable so an existing on-card profile still
// appears in the list rather than silently vanishing.
//
// **These files carry no name in the payload at all.** Verified against both
// files on the test card: `Player.sav` and `profile.sav` are byte-identical
// and hold exactly three non-zero bytes -- 0x01 at 0xB09A, 0x01 at 0xB09E and
// 0x01 at 0xBB76 -- with every other byte zero. The build that wrote them is
// no longer in this tree, and the only identity they carry is the file name.
//
// The previous reader seeked a hand-summed GameCube name offset of 47,990 and
// then required a full 32-byte read. That is four bytes past the end of a
// 48,018-byte file, so `fread` returned 28 and **every** legacy profile was
// silently dropped (TASKS.md #161). 47,990 is 0xBB76 besides -- one of those
// three flag bytes, not a name field. Both halves of that offset were wrong,
// so it is gone rather than adjusted.
#define LEGACY_TOTAL_SIZE 48018

// An all-but-empty legacy buffer is not a profile and must not enter the list.
// Both files on the test card hold exactly three non-zero bytes in 48,018, so
// loading one produced a profile byte-identical to profile_default_player1()
// but labelled with the file name -- and, worse, their mere presence pushed
// profile_list() past 1, which is the arm of ListDone__18MemoryCardMainMenu
// (Speed.elf 0x800F0200) that shows MC_List.fng instead of loading the single
// real save. That is what left the session on "Player 1" with the real save
// sitting right there on the card.
//
// The threshold is deliberately far from both ends of the measured range: a
// real GameCube UserProfile spends up to 31 non-zero bytes on the name alone
// and thousands more on its car and event-record arrays, while these
// placeholders carry three. Nothing between 3 and 64 has ever been observed.
#define LEGACY_MIN_NONZERO 64

// Takes the name from the file-name stem (`Player.sav` -> "Player") after
// confirming the file is exactly the size that writer produced and that it
// actually holds something. Nothing inside the payload is decoded, because
// there is nothing in it to decode -- and per this project's rule, a field
// with no recoverable value is not invented.
static bool readLegacyName(const char* path, char out[32]) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long len = ftell(f);
	if (len != (long)LEGACY_TOTAL_SIZE) { fclose(f); return false; }

	// Counted in 4 KiB chunks rather than by slurping 48 KiB, since this runs
	// once per candidate file during the boot scan.
	fseek(f, 0, SEEK_SET);
	unsigned char chunk[4096];
	long nonZero = 0;
	size_t got;
	while (nonZero < LEGACY_MIN_NONZERO && (got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
		for (size_t i = 0; i < got; i++)
			if (chunk[i]) nonZero++;
	}
	fclose(f);
	if (nonZero < LEGACY_MIN_NONZERO) {
		pslogf("profile_list: skipping empty legacy file %s (%ld non-zero byte(s))",
			path, nonZero);
		return false;
	}

	const char* base = strrchr(path, '/');
	base = base ? base + 1 : path;
	size_t n = strlen(base);
	if (n > 4) n -= 4; // drop the ".sav" the caller already matched
	if (n == 0 || n > 31) return false;
	memset(out, 0, 32);
	memcpy(out, base, n);
	return true;
}

// Reads just the name field out of a real save without decoding the rest --
// the container is still fully validated first, so a corrupt file never
// reaches the list.
static bool readRealName(const char* path, char out[32]) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len != (long)PROFILE_FILE_SIZE) { fclose(f); return false; }
	unsigned char* data = (unsigned char*)malloc((size_t)len);
	if (!data) { fclose(f); return false; }
	size_t got = fread(data, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) { free(data); return false; }

	const unsigned char* payload = mc02Payload(data, (size_t)len, path);
	if (!payload) { free(data); return false; }
	memcpy(out, payload + OFF_PROFILENAME, 32);
	out[31] = '\0';
	free(data);
	return out[0] != '\0';
}

int profile_list(ProfileEntry* out, int maxEntries) {
	int count = 0;

	// 1. the real layout: <root>/<Name>/<Name>
	DIR* d = opendir(s_saveRoot);
	if (d) {
		struct dirent* ent;
		while (count < maxEntries && (ent = readdir(d)) != NULL) {
			if (ent->d_name[0] == '.') continue;
			// Collectibles are sibling directories named "<Profile> Magazine <N>"
			// / "<Profile> DVD <N>" holding a 1 MiB cover image, not profiles.
			// They are skipped by the container check below (their payload is
			// 0x100000, not 0xD692) rather than by name matching, so an odd
			// profile name can never knock one out of -- or into -- this list.
			char path[PROFILE_PATH_MAX];
			snprintf(path, sizeof(path), "%s/%s/%s", s_saveRoot, ent->d_name, ent->d_name);
			char name[32];
			if (!readRealName(path, name)) {
				// #235: a save torn mid-write lists by its whole mirror.
				char mirror[PROFILE_PATH_MAX + 8];
				snprintf(mirror, sizeof(mirror), "%s" PROFILE_MIRROR_EXT, path);
				if (!readRealName(mirror, name)) continue;
			}

			memcpy(out[count].name, name, 32);
			strncpy(out[count].path, path, sizeof(out[count].path) - 1);
			out[count].path[sizeof(out[count].path) - 1] = '\0';
			out[count].legacy = false;
			count++;
		}
		closedir(d);
	} else {
		pslogf("profile_list: %s doesn't exist yet", s_saveRoot);
	}

	// 2. legacy files this port wrote before the real format was decoded
	d = opendir(LEGACY_PROFILE_DIR);
	if (d) {
		struct dirent* ent;
		while (count < maxEntries && (ent = readdir(d)) != NULL) {
			size_t len = strlen(ent->d_name);
			if (len < 5 || strcmp(ent->d_name + len - 4, ".sav") != 0) continue;
			char path[PROFILE_PATH_MAX];
			snprintf(path, sizeof(path), "%s/%s", LEGACY_PROFILE_DIR, ent->d_name);
			char name[32];
			if (!readLegacyName(path, name)) continue;

			memcpy(out[count].name, name, 32);
			strncpy(out[count].path, path, sizeof(out[count].path) - 1);
			out[count].path[sizeof(out[count].path) - 1] = '\0';
			out[count].legacy = true;
			count++;
		}
		closedir(d);
	}

	pslogf("profile_list: found %d profile(s) under %s (+legacy)", count, s_saveRoot);
	for (int i = 0; i < count; i++)
		pslogf("profile_list:   [%d] \"%s\" %s%s", i, out[i].name, out[i].path,
			out[i].legacy ? "  (legacy)" : "");
	return count;
}

// ---------------------------------------------------------------------------
// delete
// ---------------------------------------------------------------------------

// Removes every file in `dir`, then `dir` itself. Profile and cover
// directories hold only files, so this does not recurse.
static bool removeDirFiles(const char* dir) {
	DIR* d = opendir(dir);
	if (!d) return false;
	struct dirent* ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
		char path[PROFILE_PATH_MAX + 64];
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		if (remove(path) != 0) pslogf("profile_delete: remove(%s) failed", path);
	}
	closedir(d);
	if (rmdir(dir) != 0) {
		pslogf("profile_delete: rmdir(%s) failed", dir);
		return false;
	}
	return true;
}

// True for "<name> Magazine <N>" / "<name> DVD <N>": a cover the profile
// collected (main.c's showcase saves; see profile_save_cover).
static bool isCoverOf(const char* ent, const char* name) {
	const size_t n = strlen(name);
	if (strncmp(ent, name, n) != 0 || ent[n] != ' ') return false;
	const char* r = ent + n + 1;
	if (strncmp(r, "Magazine ", 9) == 0) r += 9;
	else if (strncmp(r, "DVD ", 4) == 0) r += 4;
	else return false;
	if (!*r) return false;
	for (; *r; r++)
		if (*r < '0' || *r > '9') return false;
	return true;
}

// MemoryCard::Delete (SpeedPS.elf 0x8010607C): the profile's save and, via
// FEPkgMemcardScreen::BuildDeleteList (0x800F07F4), every memory card file
// whose name starts with the profile's -- its collected covers. Here: the
// <root>/<Name> directory (the save, its .bak, .mirror and .display) and the
// "<Name> Magazine N" / "<Name> DVD N" directories; a legacy entry is its one
// .sav file.
bool profile_delete(const ProfileEntry* e) {
	if (!e || !e->name[0]) return false;
	if (e->legacy) {
		const bool ok = remove(e->path) == 0;
		pslogf("profile_delete: legacy %s %s", e->path, ok ? "removed" : "remove failed");
		return ok;
	}
	char dir[PROFILE_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/%s", s_saveRoot, e->name);
	const bool ok = removeDirFiles(dir);
	int covers = 0;
	DIR* d = opendir(s_saveRoot);
	if (d) {
		// Collected first: removing entries while readdir walks the same
		// directory is not safe on every devoptab.
		char found[32][64];
		int nf = 0;
		struct dirent* ent;
		while (nf < 32 && (ent = readdir(d)) != NULL)
			if (strlen(ent->d_name) < 64 && isCoverOf(ent->d_name, e->name))
				strcpy(found[nf++], ent->d_name);
		closedir(d);
		for (int i = 0; i < nf; i++) {
			char cdir[PROFILE_PATH_MAX + 64];
			snprintf(cdir, sizeof(cdir), "%s/%s", s_saveRoot, found[i]);
			if (removeDirFiles(cdir)) covers++;
		}
	}
	pslogf("profile_delete: \"%s\" %s, %d cover(s) removed", e->name,
		ok ? "removed" : "remove failed", covers);
	return ok;
}

// ---------------------------------------------------------------------------
// defaults / creation
// ---------------------------------------------------------------------------
void profile_default_player1(Profile* out) {
	memset(out, 0, sizeof(*out));
	strcpy(out->name, "Player 1");
	out->currentCarSlot = -1;
	out->isLoaded = false;
	// Default__11UserProfile (SPEED2.EXE 0x515AA0): OptionsSettings::Default
	// (0x5025B0, English), CareerSettings 1, Autosave on Prompt. Everything
	// else but the career car records below stays zero: a brand-new
	// UserProfile has no car, no cash and no career progress.
	options_default(&out->options, 0);
	out->careerSettings = 1;
	out->autosave = true;
	out->autosaveNoPrompt = false;
	// And PlayerCareerState::Default's career cars, the one part of a new
	// profile's career that is not zero.
	defaultCareerCars(out);
	out->currentCarSlot = -1;
	// And the tuned-car records' CustomPhysicsTunings, 100 each, as every
	// unused tuned record in a save holds them.
	memset(out->tunedTuning, PROFILE_TUNING_DEFAULT, sizeof(out->tunedTuning));
	// And its world map filters, every zone type shown.
	memset(out->mapFilter, 1, sizeof(out->mapFilter));
	// And its career clock, -1 (SPEED2.EXE 0x53283D).
	out->careerTime = -1.0f;
}

bool profile_save_create(const char* name, unsigned startCash) {
	char nameBuf[32];
	memset(nameBuf, 0, sizeof(nameBuf));
	strncpy(nameBuf, name, PROFILE_NAME_MAX);
	if (nameBuf[0] == '\0') {
		pslog("profile_save_create: refusing to create an unnamed profile");
		return false;
	}

	unsigned char* payload = (unsigned char*)calloc(1, PROFILE_PAYLOAD_SIZE);
	if (!payload) {
		pslog("profile_save_create: calloc failed");
		return false;
	}
	// SetProfileName__11UserProfilePCcb: zero the 32-byte field, copy the
	// name in. The rest is Default__11UserProfile's as profile_default_player1
	// has it: the options blocks and the EA Trax table, CareerSettings 1,
	// Autosave on Prompt (0x515AFB: +0xA0F8 = 1, +0xA0F9 = 0) and the career
	// car records. Every other field of a fresh profile stays at zero.
	memcpy(payload + OFF_PROFILENAME, nameBuf, 32);
	ProfileOptions opts;
	options_default(&opts, 0);
	writeOptions(payload, &opts);
	wrU32(payload, OFF_CAREERSETTINGS, 1);
	payload[OFF_AUTOSAVE] = 1;
	// PlayerCareerState::Default's career cars (defaultCareerCars above),
	// written the way profile_save_career() writes them.
	for (int i = 0; i < PROFILE_NUM_CAREER_SLOTS; i++) {
		const size_t rec = OFF_CAREERCARS + (size_t)i * STRIDE_CAREER;
		wrU32(payload, rec + REC_NAMEHASH, profile_career_slot_hash(i));
		wrU32(payload, rec + REC_INUSE, 1);
		wrU32(payload, rec + REC_FILTER, PROFILE_FILTER_CAREER);
		memset(payload + rec + REC_CAREER_TUNING, REC_CAREER_TUNING_DEF, REC_CAREER_TUNING_N);
		payload[rec + REC_CAREER_LOCKED] = i != 0;
	}
	wrU32(payload, OFF_NUM_CAREERCARS, PROFILE_NUM_CAREER_SLOTS);
	wrU32(payload, OFF_CAREERBODY + B_CURRENT_CAR, profile_ea_hash("CAREER_0"));
	// PlayerCareerState::Default's wallet: the cheat codes' bonus (#446).
	wrU32(payload, OFF_CAREERBODY + B_CASH, startCash);
	// PlayerCareerState::Default's world map filters: every type shown.
	memset(payload + OFF_CAREERTAIL, 1, CAREERTAIL_FILTERS);
	// And its career clock, -1.0f (SPEED2.EXE 0x53283D).
	wrU32(payload, OFF_CAREERBODY + B_CAREER_TIME, 0xBF800000u);

	// MC02 container (the inverse of FUN_006fc226).
	unsigned char blockA[PROFILE_MC02_BLOCKA];
	wrU32(blockA, 0, 0x10D);
	wrU32(blockA, 4, PROFILE_PAYLOAD_SIZE);

	unsigned char hdr[PROFILE_MC02_HDR_SIZE];
	wrU32(hdr, 0x00, PROFILE_MC02_MAGIC);
	wrU32(hdr, 0x04, PROFILE_FILE_SIZE);
	wrU32(hdr, 0x08, PROFILE_MC02_BLOCKA);
	wrU32(hdr, 0x0C, PROFILE_PAYLOAD_SIZE);
	wrU32(hdr, 0x10, mcCrc(blockA, sizeof(blockA)));
	wrU32(hdr, 0x14, mcCrc(payload, PROFILE_PAYLOAD_SIZE));
	wrU32(hdr, 0x18, mcCrc(hdr, 0x18));

	char dir[PROFILE_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/%s", s_saveRoot, nameBuf);
	mkdirChain(dir);

	char path[PROFILE_PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dir, nameBuf);

	FILE* f = fopen(path, "wb");
	if (!f) {
		pslogf("profile_save_create: fopen(%s) failed", path);
		free(payload);
		return false;
	}
	size_t written = fwrite(hdr, 1, sizeof(hdr), f);
	written += fwrite(blockA, 1, sizeof(blockA), f);
	written += fwrite(payload, 1, PROFILE_PAYLOAD_SIZE, f);
	fclose(f);
	free(payload);

	if (written != (size_t)PROFILE_FILE_SIZE) {
		pslogf("profile_save_create: short write to %s (%u/%u)",
			path, (unsigned)written, (unsigned)PROFILE_FILE_SIZE);
		return false;
	}
	pslogf("profile_save_create: wrote %s (%u bytes, MC02, profile \"%s\")",
		path, (unsigned)written, nameBuf);
	return true;
}

// ---------------------------------------------------------------------------
// write-back
// ---------------------------------------------------------------------------

// Whole-file read for the write-back; NULL on any failure, having logged it.
static unsigned char* readWhole(const char* path, size_t* outLen) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		pslogf("profile_save_career: fopen(%s) failed", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char* data = (len > 0 && len <= (long)(PROFILE_FILE_SIZE * 2))
		? (unsigned char*)malloc((size_t)len) : NULL;
	if (!data || fread(data, 1, (size_t)len, f) != (size_t)len) {
		pslogf("profile_save_career: could not read %s", path);
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*outLen = (size_t)len;
	return data;
}

static bool writeWhole(const char* path, const unsigned char* data, size_t len) {
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	const bool ok = fwrite(data, 1, len, f) == len;
	return fclose(f) == 0 && ok;
}

// #235: rewrite a file of the same size where it lies, so the card's FAT
// hands out no new clusters (a fresh file searched ~2 s per 32 KB cluster on
// the user's card in #233). Only a missing file, or one of another size, is
// created / truncated -- once.
static bool writeInPlace(const char* path, const unsigned char* data, size_t len) {
	FILE* f = fopen(path, "r+b");
	if (f) {
		fseek(f, 0, SEEK_END);
		if (ftell(f) != (long)len) {
			fclose(f);
			f = NULL;
		} else {
			fseek(f, 0, SEEK_SET);
		}
	}
	if (!f)
		return writeWhole(path, data, len);
	bool ok = fwrite(data, 1, len, f) == len;
	ok = fflush(f) == 0 && ok;
	return fclose(f) == 0 && ok;
}

bool profile_save_career(Profile* p) {
	if (!p->isLoaded || !p->sourcePath[0]) {
		pslog("profile_save_career: no loaded save to write back to");
		return false;
	}
	size_t len = 0;
	unsigned char* data = readWhole(p->sourcePath, &len);
	if (!data)
		return false;
	// The file on the card has to still be the save this profile came from:
	// anything mc02Payload() would refuse is left exactly as it is.
	const unsigned char* payloadC = mc02Payload(data, len, p->sourcePath);
	if (!payloadC) {
		free(data);
		return false;
	}

	// One copy of the save as it was before the port first wrote to it, kept
	// beside it and never overwritten.
	char path[PROFILE_PATH_MAX + 8];
	snprintf(path, sizeof(path), "%s.bak", p->sourcePath);
	FILE* probe = fopen(path, "rb");
	if (probe) {
		fclose(probe);
	} else if (!writeWhole(path, data, len)) {
		pslogf("profile_save_career: could not write the backup %s -- not saving", path);
		free(data);
		return false;
	}

	// The fields the career writes as it plays: the wallet, the state
	// records and the spawn point (PlayerCareerState +0x7E40, which
	// NotifyEnterEvent and SaveSpawnPosition both set).
	unsigned char* b = data + (payloadC - data);
	const size_t body = OFF_CAREERBODY;
	wrU32(b, body + B_CASH, p->cash);
	wrU32(b, body + B_NUM_RECORDS, p->stateRecCount);
	for (unsigned int i = 0; i < p->stateRecCount; i++) {
		const size_t r = body + B_STATE_RECORDS + (size_t)i * 8;
		wrU32(b, r, p->stateRec[i].eventId);
		b[r + 4] = p->stateRec[i].type;
		b[r + 5] = p->stateRec[i].pad;
		b[r + 6] = p->stateRec[i].state;
		b[r + 7] = p->stateRec[i].pad2;
	}
	for (int k = 0; k < 3; k++) {
		unsigned int v;
		memcpy(&v, &p->respawnPos[k], 4);
		wrU32(b, body + B_RESPAWN_POS + (size_t)k * 4, v);
	}
	wrU32(b, body + B_RACES_ENTERED, p->racesEntered);
	// ClaimBankReward's flags (#293).
	memcpy(b + body + B_BANK_FLAGS, p->bankFlags, PROFILE_NUM_BANK_FLAGS);
	// The stage 0 flags BuildListOfEvents gates the prologue's races on.
	b[body + B_STAGE0_TIMEOUT] = p->stage0Timeout ? 1 : 0;
	b[body + B_CS_SEE_EVENTS]  = p->csCanSeeEvents ? 1 : 0;
	b[body + B_RESPAWN_VALID]       = p->respawnValid ? 1 : 0;
	b[body + B_RESPAWN_HEADING]     = (unsigned char)(p->respawnHeading & 0xFF);
	b[body + B_RESPAWN_HEADING + 1] = (unsigned char)(p->respawnHeading >> 8);
	{
		unsigned int v;
		memcpy(&v, &p->careerTime, 4);
		wrU32(b, body + B_CAREER_TIME, v);
	}
	// The car the career drives. CommitSwitchToOwnedCar (Speed.elf 0x801272A8),
	// the crib car select's OK, stores the record's name hash and sets the
	// has-a-car flag beside it; nothing else in the port changes either.
	b[body + B_HAS_CAREER_CAR] = p->hasCareerCar ? 1 : 0;
	wrU32(b, body + B_CURRENT_CAR, p->currentCarHash);
	// DoWhatNext's flag, set when the first career movie has played.
	b[body + B_CAREER_STARTED] = p->careerStarted ? 1 : 0;
	// The stage the career is on: NotifyStageCleared (0x80125FB0) moves it
	// one on when the stage it is on clears.
	wrU32(b, body + B_CAREER_STAGE, p->careerStage);

	// The tuned cars (#379): the main menu's CUSTOMIZE adds, edits and deletes
	// them (AddNewTunedCar, the garage at entry point 3, DeleteCurrentCar).
	for (int i = 0; i < PROFILE_NUM_TUNED_SLOTS; i++) {
		const size_t rec = OFF_TUNEDCARS + (size_t)i * STRIDE_TUNED;
		const ProfileCarRecord* c = &p->tunedCar[i];
		if (!c->nameHash) {
			if (p->tunedCarWrite[i] & PROFILE_CAREER_WRITE_FILLED)
				memset(b + rec, 0, STRIDE_TUNED);
			continue;
		}
		unsigned int perf;
		memcpy(&perf, &c->perf, 4);
		wrU32(b, rec + REC_NAMEHASH, c->nameHash);
		wrU32(b, rec + REC_PERF, perf);
		wrU32(b, rec + REC_INUSE, c->inUse);
		wrU32(b, rec + REC_FILTER, c->filter);
		wrU32(b, rec + REC_HASRIDE, c->hasRide);
		for (unsigned int s = 0; s < PROFILE_CAR_SLOTS; s++)
			wrU32(b, rec + REC_SLOTARRAY + (size_t)s * 4, p->tunedParts[i][s]);
		if (p->tunedCarWrite[i] & PROFILE_CAREER_WRITE_FILLED) {
			memset(b + rec + REC_CAREER_PERF_SPEC, 0, REC_CAREER_PERF_SPEC_N);
			memset(b + rec + REC_CAREER_TUNING, REC_CAREER_TUNING_DEF, REC_CAREER_TUNING_N);
		}
		memcpy(b + rec + REC_CAREER_TUNING, p->tunedTuning[i], REC_CAREER_TUNING_N);
		const size_t spec = rec + REC_CAREER_PERF_SPEC;
		for (int u = 0; u < PROFILE_NUM_UPGRADE_PATHS; u++) {
			unsigned int w;
			memcpy(&w, &p->tunedUpgradePath[i][u], 4);
			wrU32(b, spec + SPEC_UPGRADES + (size_t)u * 4, w);
		}
		memcpy(b + spec + SPEC_INSTALLED, p->tunedInstalledParts[i], PROFILE_NUM_PERF_PARTS);
	}

	// The career cars, whole. Each slot's locked byte is NotifyChooseSponsor's
	// UnlockACareerCarSlot (0x80134048), which clears the first one set; the
	// rest of a record is the car lot's -- CommitChooseCar (0x801272B8) and
	// CareerCarTrade (0x80133FF4) both end in ChooseCarForSlot (0x801348D8),
	// which is FillFromStockCar and then chosen = 1, filter 4.
	wrU32(b, OFF_NUM_CAREERCARS, p->numCareerCars);
	for (int i = 0; i < PROFILE_NUM_CAREER_SLOTS; i++) {
		const size_t rec = OFF_CAREERCARS + (size_t)i * STRIDE_CAREER;
		const ProfileCarRecord* c = &p->careerCar[i];
		unsigned int perf;
		memcpy(&perf, &c->perf, 4);
		wrU32(b, rec + REC_NAMEHASH, c->nameHash);
		wrU32(b, rec + REC_PERF, perf);
		wrU32(b, rec + REC_INUSE, c->inUse);
		wrU32(b, rec + REC_FILTER, c->filter);
		wrU32(b, rec + REC_HASRIDE, c->hasRide);
		for (unsigned int s = 0; s < PROFILE_CAR_SLOTS; s++)
			wrU32(b, rec + REC_SLOTARRAY + (size_t)s * 4, p->careerParts[i][s]);
		b[rec + REC_CAREER_LOCKED] = c->locked;
		b[rec + REC_CAREER_CHOSEN] = c->chosen;
		if (p->careerCarWrite[i] & PROFILE_CAREER_WRITE_FILLED) {
			memset(b + rec + REC_CAREER_PERF_SPEC, 0, REC_CAREER_PERF_SPEC_N);
			memset(b + rec + REC_CAREER_TUNING, REC_CAREER_TUNING_DEF, REC_CAREER_TUNING_N);
		}
		if (p->careerCarWrite[i] & PROFILE_CAREER_WRITE_TRADED)
			memset(b + rec + REC_CAREER_INVENTORY, 0, REC_CAREER_INVENTORY_N);
		// The performance half, after those two resets so that a slot the car
		// lot has just filled or traded is cleared first and then written from
		// what the profile holds -- which for such a slot is zeros (and 100s
		// for the tunings), so the reset stands.
		memcpy(b + rec + REC_CAREER_TUNING, p->careerTuning[i], REC_CAREER_TUNING_N);
		const size_t spec = rec + REC_CAREER_PERF_SPEC;
		const size_t inv = rec + REC_CAREER_INVENTORY;
		for (int u = 0; u < PROFILE_NUM_UPGRADE_PATHS; u++) {
			unsigned int w;
			memcpy(&w, &p->careerUpgradePath[i][u], 4);
			wrU32(b, spec + SPEC_UPGRADES + (size_t)u * 4, w);
		}
		memcpy(b + spec + SPEC_INSTALLED, p->careerInstalledParts[i],
			PROFILE_NUM_PERF_PARTS);
		memcpy(b + inv + INV_OWNED_BITS, p->careerOwnedParts[i],
			sizeof(p->careerOwnedParts[i]));
		wrU32(b, inv + INV_SPEND, p->careerPartsSpend[i]);
		for (int k = 0; k < PROFILE_NUM_PERF_PARTS; k++)
			wrU32(b, inv + INV_BRANDS + (size_t)k * 4, p->careerOwnedBrands[i][k]);
		// The visual runs, as read (careerVisualInv).
		static const size_t kVis[4][2] = { { 0x000, 0x15C }, { 0x16C, 0x294 },
			{ 0x298, 0x2A0 }, { 0x3B0, 0x3C0 } };
		for (int k = 0; k < 4; k++)
			memcpy(b + inv + kVis[k][0], p->careerVisualInv[i] + kVis[k][0],
				kVis[k][1] - kVis[k][0]);
	}

	// Each stage's record: its completed byte (CheckIfClearedStage,
	// 0x80124CE8, sets +0xFC), its showcases (+0xC4), its outrun wins (+0xDC,
	// RuntimeCareerStageInfo::NotifyRandomEncounterFinished 0x80124700) and
	// its sponsor record. The stage number is not written: nothing in the
	// port changes it, so the file's own stands.
	for (int s = 0; s < PROFILE_NUM_STAGES; s++) {
		const size_t r = OFF_STAGEINFO + (size_t)s * STRIDE_STAGEINFO;
		const ProfileStageInfo* st = &p->stage[s];
		b[r + STAGE_COMPLETED] = p->stageCompleted[s] ? 1 : 0;
		for (int i = 0; i < PROFILE_STAGE_WORLD_RACES; i++)
			wrU32(b, r + STAGE_WORLD_RACES + (size_t)i * 4, st->worldRace[i]);
		for (int i = 0; i < PROFILE_STAGE_SPONSOR_RACES; i++)
			wrU32(b, r + STAGE_SPONSOR_RACES + (size_t)i * 4, st->sponsorRace[i]);
		for (int i = 0; i < PROFILE_STAGE_URL_RACES; i++)
			wrU32(b, r + STAGE_URL_RACES + (size_t)i * 4, st->urlRace[i]);
		for (int i = 0; i < PROFILE_STAGE_SHOWCASES; i++)
			wrU32(b, r + STAGE_SHOWCASES + (size_t)i * 4, st->showcase[i]);
		unsigned int v;
		memcpy(&v, &st->sponsorPoints, 4);
		wrU32(b, r + STAGE_SPONSOR_PTS, v);
		wrU32(b, r + STAGE_SPONSOR_PTS_RACES, (unsigned int)st->sponsorPointRaces);
		wrU32(b, r + STAGE_OUTRUN_WINS, (unsigned int)st->outrunWins);
		for (int i = 0; i < PROFILE_STAGE_SPONSORS; i++)
			wrU32(b, r + STAGE_SPONSORS + (size_t)i * 4, st->sponsor[i]);
	}

	// The SMS queue, each message back in the slot it came from, which is why
	// ProfileSmsSlot carries the disk index it was read from: p->sms[] is
	// compacted and the file's 256 slots are not.
	//
	// For a message the file already held only the four flag bytes move:
	// NotifyReadMessage (Speed.elf 0x8012C588) sets read, NotifyDeleteMessage
	// (0x8012C610) sets deleted, MarkAllReadMessagesSortable (0x8012C3F4) sets
	// sortable, and MarkAllAutoPopupsSeen (0x8012BCC4) sets popped. The id and
	// frame words are written as well for the ones AddMessage queued since
	// (sms_add_external_message()), and are the file's own for the rest.
	unsigned int smsQueued = 0, smsUnread = 0;
	for (unsigned int i = 0; i < p->smsCount && i < PROFILE_MAX_SMS; i++) {
		const ProfileSmsSlot* m = &p->sms[i];
		if (m->slot >= SMS_SLOTS)
			continue;
		const size_t s = OFF_SMSQUEUE + (size_t)m->slot * SMS_SLOT_STRIDE;
		wrU32(b, s + SMS_SLOT_ID, m->id);
		wrU32(b, s + SMS_SLOT_BODYFRAME, m->bodyFrameId);
		wrU32(b, s + SMS_SLOT_ARG, m->argId);
		wrU32(b, s + SMS_SLOT_SUBJFRAME, m->subjectFrameId);
		b[s + SMS_SLOT_READ]     = m->read;
		b[s + SMS_SLOT_POPPED]   = m->popped;
		b[s + SMS_SLOT_DELETED]  = m->deleted;
		b[s + SMS_SLOT_SORTABLE] = m->sortable;
		smsQueued++;
		if (m->read == 0 && m->deleted != 1)
			smsUnread++;
	}
	// The two counts after the slot array. LoadFromBuffer (0x8012CC30) throws
	// the file's values away and recomputes both, so they are advisory -- they
	// are kept right anyway so a save this port wrote reads the same in the
	// retail game.
	wrU32(b, OFF_SMSQUEUE + SMS_QUEUE_COUNT, smsQueued);
	wrU32(b, OFF_SMSQUEUE + SMS_QUEUE_UNREAD, smsUnread);

	// The HighScoresDatabase records, all 320, exactly as they were loaded
	// except where profile_commit_high_score() changed one. The 0x50 bytes of
	// career totals after them (+0xA00) are left as the file has them -- the
	// port does not keep them yet (profile_save.h).
	for (int i = 0; i < PROFILE_NUM_HIGHSCORES; i++) {
		const ProfileHighScore* h = &p->highScores[i];
		const size_t r = OFF_HIGHSCORES + (size_t)i * 8;
		b[r]     = (unsigned char)((unsigned short)h->trackId & 0xFF);
		b[r + 1] = (unsigned char)(((unsigned short)h->trackId >> 8) & 0xFF);
		b[r + 2] = h->category;
		b[r + 3] = h->kind;
		wrU32(b, r + 4, (unsigned int)h->value);
	}

	// The options: everything the Options screens can change.
	writeOptions(b, &p->options);
	wrU32(b, OFF_CAREERSETTINGS, p->careerSettings);
	b[OFF_AUTOSAVE]          = p->autosave ? 1 : 0;
	b[OFF_AUTOSAVE_NOPROMPT] = p->autosaveNoPrompt ? 1 : 0;
	// The world map's filters (GameWorldMapScreen::ToggleFilter).
	memcpy(b + OFF_CAREERTAIL, p->mapFilter, CAREERTAIL_FILTERS);
	// CareerSpeech, as main.c last stored it.
	memcpy(b + OFF_CAREERSPEECH, p->speechBlock, PROFILE_SPEECH_BLOCK);

	// Block B's checksum, then the header's over it.
	const unsigned int sizeA = rdU32(data, 0x08), sizeB = rdU32(data, 0x0C);
	wrU32(data, 0x14, mcCrc(data + PROFILE_MC02_HDR_SIZE + sizeA, sizeB));
	wrU32(data, 0x18, mcCrc(data, 0x18));

	// #235: both files rewritten in place, the mirror first, so the card
	// allocates nothing after the first save and a write torn by a power cut
	// leaves one whole copy (profile_load_file falls back to the mirror).
	const u64 t0 = osGetTime();
	snprintf(path, sizeof(path), "%s" PROFILE_MIRROR_EXT, p->sourcePath);
	if (!writeInPlace(path, data, len)) {
		pslogf("profile_save_career: could not write %s -- not saving", path);
		free(data);
		return false;
	}
	const u64 t1 = osGetTime();
	bool ok = writeInPlace(p->sourcePath, data, len);
	free(data);
	if (!ok) {
		pslogf("profile_save_career: could not write %s (its mirror is whole)", p->sourcePath);
		return false;
	}
	pslogf("profile_save_career: in place, mirror %u ms + save %u ms (#235)",
		(unsigned)(t1 - t0), (unsigned)(osGetTime() - t1));
	// The resets are on the card now. Kept, they would wipe whatever a later
	// save had to say about the same slot's tuning or bought parts.
	memset(p->careerCarWrite, 0, sizeof(p->careerCarWrite));
	memset(p->tunedCarWrite, 0, sizeof(p->tunedCarWrite));
	pslogf("profile_save_career: wrote %s -- cash=$%u, %u state records, respawn %.1f,%.1f,%.1f",
		p->sourcePath, p->cash, p->stateRecCount, p->respawnPos[0], p->respawnPos[1],
		p->respawnPos[2]);
	return true;
}

bool profile_save_cover(const char* title, const unsigned char* bgra) {
	if (!title || !title[0] || !bgra) {
		pslog("profile_save_cover: no title or no picture");
		return false;
	}
	// FUN_004D9C60's block A: 0x10D and the payload's size.
	unsigned char blockA[PROFILE_MC02_BLOCKA];
	wrU32(blockA, 0, 0x10D);
	wrU32(blockA, 4, PROFILE_COVER_SIZE);

	unsigned char hdr[PROFILE_MC02_HDR_SIZE];
	wrU32(hdr, 0x00, PROFILE_MC02_MAGIC);
	wrU32(hdr, 0x04, PROFILE_MC02_DATA_OFF + PROFILE_COVER_SIZE);
	wrU32(hdr, 0x08, PROFILE_MC02_BLOCKA);
	wrU32(hdr, 0x0C, PROFILE_COVER_SIZE);
	wrU32(hdr, 0x10, mcCrc(blockA, sizeof(blockA)));
	wrU32(hdr, 0x14, mcCrc(bgra, PROFILE_COVER_SIZE));
	wrU32(hdr, 0x18, mcCrc(hdr, 0x18));

	char dir[PROFILE_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/%s", s_saveRoot, title);
	mkdirChain(dir);
	char path[PROFILE_PATH_MAX], tmp[PROFILE_PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dir, title);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	// Beside it first, as profile_save_career() writes, so a failed write
	// leaves a cover already there as it was.
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		pslogf("profile_save_cover: fopen(%s) failed", tmp);
		return false;
	}
	size_t written = fwrite(hdr, 1, sizeof(hdr), f);
	written += fwrite(blockA, 1, sizeof(blockA), f);
	written += fwrite(bgra, 1, PROFILE_COVER_SIZE, f);
	const bool closed = fclose(f) == 0;
	if (!closed || written != PROFILE_MC02_DATA_OFF + PROFILE_COVER_SIZE) {
		pslogf("profile_save_cover: short write to %s (%u/%u)", tmp, (unsigned)written,
			(unsigned)(PROFILE_MC02_DATA_OFF + PROFILE_COVER_SIZE));
		remove(tmp);
		return false;
	}
	remove(path);
	if (rename(tmp, path) != 0) {
		pslogf("profile_save_cover: rename %s -> %s failed", tmp, path);
		return false;
	}
	pslogf("profile_save_cover: wrote %s (%u bytes, MC02, crc 0x%08X)", path,
		(unsigned)written, rdU32(hdr, 0x14));
	return true;
}

unsigned char* profile_load_cover(const char* title) {
	if (!title || !title[0]) {
		pslog("profile_load_cover: no title");
		return NULL;
	}
	char path[PROFILE_PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/%s", s_saveRoot, title, title);
	FILE* f = fopen(path, "rb");
	if (!f) {
		pslogf("profile_load_cover: no %s", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char* data = len > 0 ? (unsigned char*)malloc((size_t)len) : NULL;
	const size_t got = data ? fread(data, 1, (size_t)len, f) : 0;
	fclose(f);
	if (!data || got != (size_t)len) {
		pslogf("profile_load_cover: short read on %s (%u/%ld)", path, (unsigned)got, len);
		free(data);
		return NULL;
	}
	const unsigned char* payload = mc02PayloadSized(data, (size_t)len, path,
		PROFILE_COVER_SIZE, "a cover");
	if (!payload) {
		free(data);
		return NULL;
	}
	// The payload to the front of the buffer the file was read into, which
	// the caller then owns: a second megabyte is not to be had beside it.
	memmove(data, payload, PROFILE_COVER_SIZE);
	pslogf("profile_load_cover: read %s (%ld bytes, MC02)", path, len);
	return data;
}

// The two loops below are SPEED2.EXE's, unrolled ten records at a time there
// (0x20 passes): a record counts when its s16 at +0x00 is the track and its
// byte at +0x03 the kind.
bool profile_best_time(const Profile* p, int trackId, int kind, int* out) {
	bool found = false;
	int best = 0x7FFFFFFF;
	for (int i = 0; i < PROFILE_NUM_HIGHSCORES; i++) {
		const ProfileHighScore* h = &p->highScores[i];
		if (h->trackId == trackId && h->kind == kind && (h->value < best || !found)) {
			found = true;
			best = h->value;
		}
	}
	*out = best;
	return found;
}

int profile_best_score(const Profile* p, int trackId, int kind) {
	int best = 0;
	for (int i = 0; i < PROFILE_NUM_HIGHSCORES; i++) {
		const ProfileHighScore* h = &p->highScores[i];
		if (h->trackId == trackId && h->kind == kind && best < h->value)
			best = h->value;
	}
	return best;
}

// CalcRaceTypeForHighScores (0x801322FC): the first bit set, in this order.
int profile_high_score_race_type(unsigned int mask) {
	if (mask & 0x1u) return PROFILE_HS_CIRCUIT;
	if (mask & 0x2u) return PROFILE_HS_DRAG;
	if (mask & 0x4u) return PROFILE_HS_SPRINT;
	if (mask & 0x8u) return PROFILE_HS_DRIFT;
	return PROFILE_HS_NONE;
}

// FindHighScore (0x8013127C): the record for this exact key, or NULL. Unlike
// the two scans above it reads the category byte as well, so a six-lap best
// and a three-lap best on the same track live in their own records.
static ProfileHighScore* findHighScore(Profile* p, int trackId, int kind, int category) {
	for (int i = 0; i < PROFILE_NUM_HIGHSCORES; i++) {
		ProfileHighScore* h = &p->highScores[i];
		if (h->trackId == (short)trackId && (signed char)h->kind == (signed char)kind &&
				(signed char)h->category == (signed char)category)
			return h;
	}
	return NULL;
}

// GetEmptyHighScore (0x80131434): the first record with no track in it.
static ProfileHighScore* emptyHighScore(Profile* p) {
	for (int i = 0; i < PROFILE_NUM_HIGHSCORES; i++)
		if (p->highScores[i].trackId == 0)
			return &p->highScores[i];
	return NULL;
}

// The validity test retail runs on a lap time before it will store or compare
// one: not zero and not the "never set" sentinel.
static bool lapTimeUsable(int ticks) {
	return ticks != 0 && ticks != 0x7FFFFFFF;
}

// DidPlayerBreakRecord (0x80131464). Drift wants a bigger number, everything
// else a smaller one; an unusable time beats nothing.
static bool brokeRecord(int raceType, const ProfileHighScore* rec, int bestLapTicks,
		int driftScore) {
	if (raceType == PROFILE_HS_DRIFT)
		return (float)driftScore > (float)rec->value;
	if (!lapTimeUsable(bestLapTicks))
		return false;
	return bestLapTicks < rec->value;
}

int profile_commit_high_score(Profile* p, int raceType, int trackId, int kind,
		int category, int bestLapTicks, int driftScore) {
	ProfileHighScore* rec = findHighScore(p, trackId, kind, category);
	int wrote = 0;
	if (rec) {
		wrote = brokeRecord(raceType, rec, bestLapTicks, driftScore) ? 1 : 0;
	} else {
		rec = emptyHighScore(p);
		if (!rec)
			return 0;
		// Retail checks the value is worth a record before it claims a slot:
		// a lap time has to be usable and has to still read as non-zero once
		// it is in seconds (x 1/4000), and a drift score may not be negative.
		bool ok = true;
		if (raceType != PROFILE_HS_DRIFT) {
			if (!lapTimeUsable(bestLapTicks) || (float)bestLapTicks * 0.00025f == 0.0f)
				ok = false;
		}
		if (raceType == PROFILE_HS_DRIFT && (float)driftScore < 0.0f)
			ok = false;
		if (!ok)
			return 0;
		rec->trackId = (short)trackId;
		rec->kind = (unsigned char)kind;
		rec->category = (unsigned char)category;
		wrote = 1;
	}
	if (!wrote)
		return 0;
	if (raceType == PROFILE_HS_DRIFT) {
		rec->value = driftScore;
	} else if (lapTimeUsable(bestLapTicks)) {
		rec->value = bestLapTicks;
	}
	pslogf("high score: track %d kind %d category %d -- %s %d", trackId, kind, category,
		raceType == PROFILE_HS_DRIFT ? "score" : "best lap (ticks)", rec->value);
	return 1;
}
