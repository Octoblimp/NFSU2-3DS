#include "eventdb.h"
#include "bunidx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See eventdb.h for where the table lives and how the engine indexes it.
#define EVENTDB_CONTAINER_TYPE 0x80034A10u
#define EVENTDB_CHUNK_TYPE 0x00034A11u
#define EVENTDB_RECORD_SIZE 0x88 // the stride SPEED2.EXE:0x512C00 divides by

// The sibling shop table; see eventdb.h for how its two field offsets were
// pinned down against the shipped data.
#define SHOPDB_CHUNK_TYPE 0x00034A12u
#define SHOPDB_RECORD_SIZE 0xA0 // the stride Speed.elf 0x80121F54 walks by
#define SH_OFF_NAME 0x00
#define SH_OFF_DATA_HANDLE 0x38
#define SH_OFF_HANDLE 0x3C
#define SH_OFF_PLATFORM 0x40
#define SH_OFF_KIND 0x50
// What IsShopEnterable (Speed.elf 0x801243B8) and IsShopUnlocked (0x80122044)
// read; see ShopRec in eventdb.h.
#define SH_OFF_NEEDS_EVENT 0x51
#define SH_OFF_UNLOCK_ARG  0x74
#define SH_OFF_UNLOCK_KIND 0x9C
#define SH_OFF_STAGE       0x9D
#define SH_OFF_MOVIE       0x20

// The career string blob, sub-chunk 0x34A1D of the same container. Event names
// are not stored inline: each record holds a u16 offset into this blob, which
// GetCareerStringAtOffset__FUs (Speed.elf 0x801215B0) adds to the blob base --
// the whole of that function is `return base + offset`. LoaderMostWantedCareer-
// Chunk (0x80123520) is what parks the blob's address there, and the same
// switch is where every sub-chunk id in eventdb.h was read from.
#define STRDB_CHUNK_TYPE 0x00034A1Du

// The per-stage data, sub-chunk 0x34A18, stride 0x50 -- see StageRec.
#define STAGEDB_CHUNK_TYPE 0x00034A18u
#define STAGEDB_RECORD_SIZE 0x50
#define ST_OFF_STAGE 0x00
#define ST_OFF_SPONSOR_COUNT 0x01
#define ST_OFF_ENCOUNTER_PURSE 0x02
#define ST_OFF_STAR_PURSE 0x04
#define ST_OFF_SPONSORS 0x08
#define ST_OFF_REQ_WORLD_RACES 0x1C
#define ST_OFF_CLEAR_EVENT 0x28
#define ST_OFF_AI_RACES 0x42
#define ST_OFF_AI_PERF_LO 0x44
#define ST_OFF_AI_PERF_HI 0x48
#define ST_OFF_MAX_BY_KIND 0x30
#define ST_OFF_HIDDEN_CAP 0x41
#define ST_OFF_OUTRUN_CAP 0x40

// The car-unlock table, sub-chunk 0x34A1F, 12 bytes a record -- see
// CarUnlockRec.
#define UNLOCKDB_CHUNK_TYPE 0x00034A1Fu
#define UNLOCKDB_RECORD_SIZE 0x0C

// The showcase table, sub-chunk 0x34A16, 0x40 bytes a record -- see
// ShowcaseRec.
#define SHOWCASEDB_CHUNK_TYPE 0x00034A16u
#define SHOWCASEDB_RECORD_SIZE 0x40
#define SC_OFF_NAME 0x00
#define SC_OFF_HANDLE 0x20
#define SC_OFF_TYPE 0x24
#define SC_OFF_STAGE 0x25
#define SC_OFF_FIELD26 0x26
#define SC_OFF_MAG 0x28
#define SC_OFF_TRIGGER 0x2C
#define SC_OFF_SNAPSHOT 0x30
#define SC_OFF_COND 0x34
#define SC_OFF_REQUIRED 0x35
#define SC_OFF_FIELD36 0x36
#define SC_OFF_COND_ARG 0x38
#define SC_OFF_STAR 0x3C

// The world challenge table, sub-chunk 0x34A1B, 0x18 bytes a record -- see
// WorldChallengeRec.
#define CHALLENGEDB_CHUNK_TYPE 0x00034A1Bu
#define CHALLENGEDB_RECORD_SIZE 0x18
#define WC_OFF_NAME 0x00
#define WC_OFF_START 0x02
#define WC_OFF_STAGE 0x04
#define WC_OFF_UNLOCK_KIND 0x06
#define WC_OFF_UNLOCK_COUNT 0x07
#define WC_OFF_SMS 0x08
#define WC_OFF_GOAL 0x0C
#define WC_OFF_TIME 0x10
#define WC_OFF_TYPE 0x14
#define WC_OFF_SUB 0x15

// The bank pickups, sub-chunk 0x34A1E, 12 bytes a record -- see
// BankTriggerRec.
#define BANKDB_CHUNK_TYPE 0x00034A1Eu
#define BANKDB_RECORD_SIZE 0x0C

// The sponsor table, sub-chunk 0x34A19, 16 bytes a record -- see SponsorRec.
#define SPONSORDB_CHUNK_TYPE 0x00034A19u
#define SPONSORDB_RECORD_SIZE 0x10
#define SP_OFF_NAME 0x00
#define SP_OFF_PURSE 0x02
#define SP_OFF_RACE_TYPES 0x04
#define SP_OFF_FIELD8 0x08
#define SP_OFF_BONUS 0x0C
#define SP_OFF_MIN_AVG 0x0E

// The SMS message table, sub-chunk 0x34A17 of the same container --
// LoaderMostWantedCareerChunk's own arm for it (Speed.elf 0x801236A4) hands
// the payload and its size to CreateSMSMessageDataTable. See SmsRec.
#define SMSDB_CHUNK_TYPE 0x00034A17u
#define SMSDB_RECORD_SIZE 0x14 // the stride CreateSMSMessageDataTable divides by
#define SM_OFF_NAME 0x00
#define SM_OFF_TRIGGER 0x02
#define SM_OFF_CATEGORY 0x03
#define SM_OFF_AUTOPOPUP 0x04
#define SM_OFF_ENVELOPE 0x05
#define SM_OFF_FIELD6 0x06
#define SM_OFF_FIELD7 0x07
#define SM_OFF_ARG0 0x08
#define SM_OFF_ARG1 0x0C
#define SM_OFF_FROM 0x10

// Record field offsets, all confirmed against the shipped PC GLOBALB.BUN.
#define EV_OFF_NAME 0x00
#define EV_OFF_PRE_MOVIE 0x02
#define EV_OFF_POST_MOVIE 0x04
#define EV_OFF_EVENTID 0x08
#define EV_OFF_FIELD10 0x10
#define EV_OFF_ISSUV 0x0D
#define EV_OFF_URL_RULE 0x0C
#define EV_OFF_TYPE 0x0F
#define EV_OFF_TRACKS 0x18
#define EV_OFF_TRIGGER 0x28
#define EV_OFF_PRESET_CAR 0x2C
#define EV_OFF_PURSE 0x30
#define EV_OFF_CLASS 0x34
#define EV_OFF_SHOP 0x3C
#define EV_OFF_TRAFFIC 0x36
#define EV_OFF_STAGE 0x37
#define EV_OFF_TRACKCOUNT 0x7E
#define EV_OFF_HIDDEN 0x7F
// +0x40 is the first CareerEventOpponentData, whose own +0x00 is a u16 offset
// into the same career string blob: GetName__23CareerEventOpponentData
// (Speed.elf 0x80121AA0) is an lhz followed by GetCareerStringAtOffset and
// nothing else. The stride is 0x0C -- AddCareerEventAIOpponents (0x8011A22C)
// walks the array with `iVar14 = iVar14 + 0xc` -- and five of them reach
// +0x40 + 5*0x0C = +0x7C, which is the count itself. See EventOpponent in
// eventdb.h for the field-by-field decode.
#define EV_OFF_OPP0_NAME 0x40
#define EV_OPPONENT_SIZE 0x0C
#define EV_OPP_OFF_NAME 0x00
#define EV_OPP_OFF_MATCH 0x02
#define EV_OPP_OFF_MATCHVALUE 0x03
#define EV_OPP_OFF_CAR 0x04
#define EV_OPP_OFF_SKILL 0x08
#define EV_OPP_OFF_FLAG 0x0B
#define EV_OFF_NUMOPPONENTS 0x7C
#define EV_OFF_SENDER 0x84

static u32 rd32le(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static s16 rd16le(const u8* p) {
	return (s16)(u16)((u32)p[0] | ((u32)p[1] << 8));
}

// One string out of the career blob, by the u16 offset a record holds. An
// offset that runs past the blob -- or a file with no blob at all -- leaves the
// string empty rather than failing the load: these are the names the log and
// the engage dialog print, and every field the game actually drives is read
// straight out of the record.
static void readBlobString(const u8* str, u32 strSize, u32 off, char* out,
		size_t outSize) {
	out[0] = '\0';
	if (!str || off >= strSize)
		return;
	u32 avail = strSize - off;
	size_t want = avail < outSize ? (size_t)avail : outSize;
	memcpy(out, str + off, want);
	if (want < outSize)
		out[want] = '\0';
	// A blob string that fills the buffer with no terminator of its own still
	// ends here; the longest shipped event name is 18 characters.
	out[outSize - 1] = '\0';
}

bool eventdb_load(const char* bunPath, EventDb* out) {
	memset(out, 0, sizeof(*out));

	// The container is the first top-level 0x80034A10 -- 0x512C00's one-shot
	// guard means the engine stops at the first one too, see eventdb.h -- and
	// it is read whole, 71 KB in one go. This used to be an 8-byte read per
	// top-level chunk to find it (it is chunk 208, 7.7 MB in) and then a read
	// per record and per string: 685 ms of every world entry (TASKS.md #270).
	long hdrOff = 0;
	u32 size = 0;
	if (!bunidx_find(bunPath, EVENTDB_CONTAINER_TYPE, &hdrOff, &size) || size < 8)
		return false;
	u8* body = (u8*)malloc(size);
	if (!body)
		return false;
	FILE* f = fopen(bunPath, "rb");
	const bool read = f && fseek(f, hdrOff + 8, SEEK_SET) == 0 &&
		fread(body, 1, size, f) == size;
	if (f)
		fclose(f);
	if (!read) {
		free(body);
		return false;
	}

	// The container's body is the same flat {u32 type, u32 size} list. The
	// walk runs it to its end rather than stopping at the first hit, because it
	// is looking for four chunks; 0x512C00 walks the whole sub-chunk list for
	// the same reason.
	const u8* ev = NULL;
	u32 evSize = 0;
	const u8* shop = NULL;
	u32 shopSize = 0;
	const u8* str = NULL;
	u32 strSize = 0;
	const u8* stage = NULL;
	u32 stageSize = 0;
	const u8* spon = NULL;
	u32 sponSize = 0;
	const u8* sms = NULL;
	u32 smsSize = 0;
	const u8* unl = NULL;
	u32 unlSize = 0;
	const u8* sc = NULL;
	u32 scSize = 0;
	const u8* wc = NULL;
	u32 wcSize = 0;
	const u8* bank = NULL;
	u32 bankSize = 0;
	for (u32 sub = 0; sub + 8 <= size; ) {
		const u32 st = rd32le(body + sub);
		const u32 ss = rd32le(body + sub + 4);
		if (st == 0 && ss == 0)
			break;
		if (ss > size - (sub + 8))
			break;
		const u8* pay = body + sub + 8;
		if (st == EVENTDB_CHUNK_TYPE) {
			ev = pay;
			evSize = ss;
		} else if (st == SHOPDB_CHUNK_TYPE) {
			shop = pay;
			shopSize = ss;
		} else if (st == STRDB_CHUNK_TYPE) {
			str = pay;
			strSize = ss;
		} else if (st == STAGEDB_CHUNK_TYPE) {
			stage = pay;
			stageSize = ss;
		} else if (st == SPONSORDB_CHUNK_TYPE) {
			spon = pay;
			sponSize = ss;
		} else if (st == SMSDB_CHUNK_TYPE) {
			sms = pay;
			smsSize = ss;
		} else if (st == UNLOCKDB_CHUNK_TYPE) {
			unl = pay;
			unlSize = ss;
		} else if (st == SHOWCASEDB_CHUNK_TYPE) {
			sc = pay;
			scSize = ss;
		} else if (st == CHALLENGEDB_CHUNK_TYPE) {
			wc = pay;
			wcSize = ss;
		} else if (st == BANKDB_CHUNK_TYPE) {
			bank = pay;
			bankSize = ss;
		}
		sub += 8 + ss;
	}

	// The shop table first, so that a container carrying only 0x34A12 still
	// fills it -- the two sub-chunks are independent, and the caller is told
	// about each by its own count.
	if (shop) {
		int shops = (int)(shopSize / SHOPDB_RECORD_SIZE);
		if (shops > EVENTDB_MAX_SHOPS)
			shops = EVENTDB_MAX_SHOPS;
		for (int i = 0; i < shops; i++) {
			const u8* rec = shop + i * SHOPDB_RECORD_SIZE;
			ShopRec* s = &out->shops[i];
			memcpy(s->name, rec + SH_OFF_NAME, sizeof(s->name) - 1);
			s->name[sizeof(s->name) - 1] = '\0';
			s->handle = rd32le(rec + SH_OFF_DATA_HANDLE);
			s->triggerHandle = rd32le(rec + SH_OFF_HANDLE);
			memcpy(s->platform, rec + SH_OFF_PLATFORM, sizeof(s->platform) - 1);
			s->platform[sizeof(s->platform) - 1] = '\0';
			s->kind = (s8)rec[SH_OFF_KIND];
			s->needsEvent = (s8)rec[SH_OFF_NEEDS_EVENT];
			s->unlockArg = rd32le(rec + SH_OFF_UNLOCK_ARG);
			s->unlockKind = (s8)rec[SH_OFF_UNLOCK_KIND];
			s->stage = (s8)rec[SH_OFF_STAGE];
			memcpy(s->movie, rec + SH_OFF_MOVIE, sizeof(s->movie) - 1);
			s->movie[sizeof(s->movie) - 1] = '\0';
		}
		out->shopCount = shops;
	}

	// The stage data, on the same terms as the shop table: independent of the
	// event table, and reported by its own count.
	if (stage) {
		int stages = (int)(stageSize / STAGEDB_RECORD_SIZE);
		if (stages > EVENTDB_MAX_STAGES)
			stages = EVENTDB_MAX_STAGES;
		for (int i = 0; i < stages; i++) {
			const u8* rec = stage + i * STAGEDB_RECORD_SIZE;
			StageRec* s = &out->stages[i];
			s->stage = (s8)rec[ST_OFF_STAGE];
			s->sponsorCount = (s8)rec[ST_OFF_SPONSOR_COUNT];
			if (s->sponsorCount > EVENTDB_STAGE_SPONSORS)
				s->sponsorCount = EVENTDB_STAGE_SPONSORS;
			if (s->sponsorCount < 0)
				s->sponsorCount = 0;
			s->encounterPurse = (u16)rd16le(rec + ST_OFF_ENCOUNTER_PURSE);
			s->starRatingPurse = (u16)rd16le(rec + ST_OFF_STAR_PURSE);
			for (int k = 0; k < EVENTDB_STAGE_SPONSORS; k++) {
				s->sponsorHash[k] = rd32le(rec + ST_OFF_SPONSORS + k * 4);
				s->reqWorldRaces[k] = rd16le(rec + ST_OFF_REQ_WORLD_RACES + k * 2);
			}
			s->clearEvent = rd32le(rec + ST_OFF_CLEAR_EVENT);
			s->aiRaces = rd16le(rec + ST_OFF_AI_RACES);
			u32 w = rd32le(rec + ST_OFF_AI_PERF_LO);
			memcpy(&s->aiPerfLo, &w, sizeof(w));
			w = rd32le(rec + ST_OFF_AI_PERF_HI);
			memcpy(&s->aiPerfHi, &w, sizeof(w));
			for (int k = 0; k < 8; k++)
				s->maxByKind[k] = (s8)rec[ST_OFF_MAX_BY_KIND + k];
			s->hiddenCap = (s8)rec[ST_OFF_HIDDEN_CAP];
			s->outrunCap = (s8)rec[ST_OFF_OUTRUN_CAP];
		}
		out->stageCount = stages;
	}

	// The bank pickups (#293), LoadBankTriggers' records as they are.
	if (bank) {
		int n = (int)(bankSize / BANKDB_RECORD_SIZE);
		if (n > EVENTDB_MAX_BANK_TRIGGERS)
			n = EVENTDB_MAX_BANK_TRIGGERS;
		for (int i = 0; i < n; i++) {
			const u8* rec = bank + i * BANKDB_RECORD_SIZE;
			BankTriggerRec* t = &out->banks[i];
			t->amount = (u16)(rec[0] | rec[1] << 8);
			t->type = rec[2];
			t->flag = rec[3];
			t->param = rd32le(rec + 4);
			t->zoneHash = rd32le(rec + 8);
		}
		out->bankCount = n;
	}

	// The car unlocks, independent like the rest.
	if (unl) {
		int n = (int)(unlSize / UNLOCKDB_RECORD_SIZE);
		if (n > EVENTDB_MAX_CAR_UNLOCKS)
			n = EVENTDB_MAX_CAR_UNLOCKS;
		for (int i = 0; i < n; i++) {
			const u8* rec = unl + i * UNLOCKDB_RECORD_SIZE;
			CarUnlockRec* u = &out->carUnlocks[i];
			u->carHash = rd32le(rec + 0);
			u->eventId[0] = rd32le(rec + 4);
			u->eventId[1] = rd32le(rec + 8);
		}
		out->carUnlockCount = n;
	}

	// The showcases, the same again.
	if (sc) {
		int n = (int)(scSize / SHOWCASEDB_RECORD_SIZE);
		if (n > EVENTDB_MAX_SHOWCASES)
			n = EVENTDB_MAX_SHOWCASES;
		for (int i = 0; i < n; i++) {
			const u8* rec = sc + i * SHOWCASEDB_RECORD_SIZE;
			ShowcaseRec* s = &out->showcases[i];
			memcpy(s->name, rec + SC_OFF_NAME, sizeof(s->name) - 1);
			s->name[sizeof(s->name) - 1] = '\0';
			s->handle = rd32le(rec + SC_OFF_HANDLE);
			s->type = (s8)rec[SC_OFF_TYPE];
			s->stage = (s8)rec[SC_OFF_STAGE];
			s->field26 = (u16)rd16le(rec + SC_OFF_FIELD26);
			s->magHash = rd32le(rec + SC_OFF_MAG);
			s->triggerHandle = rd32le(rec + SC_OFF_TRIGGER);
			u32 w = rd32le(rec + SC_OFF_SNAPSHOT);
			memcpy(&s->snapshotTime, &w, sizeof(w));
			s->cond = (s8)rec[SC_OFF_COND];
			s->required = rec[SC_OFF_REQUIRED];
			s->field36 = (u16)rd16le(rec + SC_OFF_FIELD36);
			s->condArg = rd32le(rec + SC_OFF_COND_ARG);
			w = rd32le(rec + SC_OFF_STAR);
			memcpy(&s->starRating, &w, sizeof(w));
		}
		out->showcaseCount = n;
	}

	// The world challenges. Both names are blob offsets, resolved here and
	// hashed once with plain bStringHash: FindWorldChallengeByTriggerZone
	// hashes the start trigger's name on every record it walks, and the
	// career keeps the challenge's state record under the hash of its name.
	if (wc) {
		int n = (int)(wcSize / CHALLENGEDB_RECORD_SIZE);
		if (n > EVENTDB_MAX_CHALLENGES)
			n = EVENTDB_MAX_CHALLENGES;
		for (int i = 0; i < n; i++) {
			const u8* rec = wc + i * CHALLENGEDB_RECORD_SIZE;
			WorldChallengeRec* c = &out->challenges[i];
			readBlobString(str, strSize, (u32)(u16)rd16le(rec + WC_OFF_NAME),
				c->name, sizeof(c->name));
			readBlobString(str, strSize, (u32)(u16)rd16le(rec + WC_OFF_START),
				c->startTrigger, sizeof(c->startTrigger));
			u32 h = 0xFFFFFFFFu;
			for (const unsigned char* p = (const unsigned char*)c->name; *p; p++)
				h = h * 33u + *p;
			c->nameHash = h;
			h = 0xFFFFFFFFu;
			for (const unsigned char* p = (const unsigned char*)c->startTrigger;
					*p; p++)
				h = h * 33u + *p;
			c->startHandle = h;
			c->stage = rd16le(rec + WC_OFF_STAGE);
			c->unlockKind = (s8)rec[WC_OFF_UNLOCK_KIND];
			c->unlockCount = (s8)rec[WC_OFF_UNLOCK_COUNT];
			c->smsHash = rd32le(rec + WC_OFF_SMS);
			c->goalHandle = rd32le(rec + WC_OFF_GOAL);
			c->timeLimit = (s32)rd32le(rec + WC_OFF_TIME);
			c->type = rec[WC_OFF_TYPE];
			for (int k = 0; k < 3; k++)
				c->sub[k] = (s8)rec[WC_OFF_SUB + k];
		}
		out->challengeCount = n;
	}

	// The SMS messages, on the same terms again. The name has to be resolved
	// here rather than kept as an offset because the save refers to a message
	// by the hash of its name, which is what the engine's own loader does:
	// GetSMSMessageDataByNameHash hashes GetMessageName(rec) on every record it
	// walks. Hashing once at load makes a mailbox open a scan over u32s.
	//
	// Both the lookup (0x8012B77C) and SMSMessageQueue::SaveToBuffer
	// (0x8012CAD8) use plain bStringHash, with no case fold. The fold
	// eventdb_feng_hash() does is FEngHashString's and makes no difference
	// here, every shipped name being upper-case already; the same helper
	// builds the <NAME>_SUBJECT and <NAME>_BODY label hashes in sms.c, where
	// the fold does matter, so one function serves both.
	if (sms) {
		int n = (int)(smsSize / SMSDB_RECORD_SIZE);
		if (n > EVENTDB_MAX_SMS)
			n = EVENTDB_MAX_SMS;
		for (int i = 0; i < n; i++) {
			const u8* rec = sms + i * SMSDB_RECORD_SIZE;
			SmsRec* m = &out->sms[i];
			readBlobString(str, strSize, (u32)(u16)rd16le(rec + SM_OFF_NAME),
				m->name, sizeof(m->name));
			m->nameHash = eventdb_feng_hash(m->name);
			m->trigger = (s8)rec[SM_OFF_TRIGGER];
			m->category = (s8)rec[SM_OFF_CATEGORY];
			m->autoPopup = rec[SM_OFF_AUTOPOPUP];
			m->envelope = rec[SM_OFF_ENVELOPE];
			m->field6 = rec[SM_OFF_FIELD6];
			m->field7 = rec[SM_OFF_FIELD7];
			m->arg0 = rd32le(rec + SM_OFF_ARG0);
			m->arg1 = rd32le(rec + SM_OFF_ARG1);
			m->fromId = rd32le(rec + SM_OFF_FROM);
		}
		out->smsCount = n;
	}

	// The sponsors, likewise independent. The name is the one string the
	// record carries, and its hash is computed once here because both the
	// stage table and the save refer to a sponsor by it.
	if (spon) {
		int n = (int)(sponSize / SPONSORDB_RECORD_SIZE);
		if (n > EVENTDB_MAX_SPONSORS)
			n = EVENTDB_MAX_SPONSORS;
		for (int i = 0; i < n; i++) {
			const u8* rec = spon + i * SPONSORDB_RECORD_SIZE;
			SponsorRec* s = &out->sponsors[i];
			readBlobString(str, strSize, (u32)(u16)rd16le(rec + SP_OFF_NAME),
				s->name, sizeof(s->name));
			u32 h = 0xFFFFFFFFu;
			for (const unsigned char* p = (const unsigned char*)s->name; *p; p++)
				h = h * 33u + *p;
			s->nameHash = h;
			s->purse = (u16)rd16le(rec + SP_OFF_PURSE);
			for (int k = 0; k < 4; k++)
				s->raceType[k] = rec[SP_OFF_RACE_TYPES + k];
			s->field8 = rd32le(rec + SP_OFF_FIELD8);
			s->bonus = rd16le(rec + SP_OFF_BONUS);
			s->minAvgPoints = rd16le(rec + SP_OFF_MIN_AVG);
		}
		out->sponsorCount = n;
	}

	if (!ev) {
		free(body);
		return false;
	}

	int count = (int)(evSize / EVENTDB_RECORD_SIZE);
	if (count > EVENTDB_MAX_EVENTS)
		count = EVENTDB_MAX_EVENTS;

	for (int i = 0; i < count; i++) {
		const u8* rec = ev + i * EVENTDB_RECORD_SIZE;
		EventRec* e = &out->recs[i];
		e->eventId = rd32le(rec + EV_OFF_EVENTID);
		e->field10 = rd32le(rec + EV_OFF_FIELD10);
		e->triggerHandle = rd32le(rec + EV_OFF_TRIGGER);
		e->purse = rd32le(rec + EV_OFF_PURSE);
		e->type = (s8)rec[EV_OFF_TYPE];
		e->cls = (s8)rec[EV_OFF_CLASS];
		e->stage = (s8)rec[EV_OFF_STAGE];
		e->traffic = (s8)rec[EV_OFF_TRAFFIC];
		e->isSUV = rec[EV_OFF_ISSUV];
		e->urlRule = (s8)rec[EV_OFF_URL_RULE];
		e->isHidden = rec[EV_OFF_HIDDEN];
		e->opponentCount = rec[EV_OFF_NUMOPPONENTS];
		e->senderStringId = rd32le(rec + EV_OFF_SENDER);
		e->shopHandle = rd32le(rec + EV_OFF_SHOP);
		e->presetCar = rd32le(rec + EV_OFF_PRESET_CAR);

		// The event's own name, and opponent 0's -- the one the engage dialog
		// signs the message with. Both are blob offsets, not inline strings.
		readBlobString(str, strSize, (u32)(u16)rd16le(rec + EV_OFF_NAME),
			e->name, sizeof(e->name));
		readBlobString(str, strSize, (u32)(u16)rd16le(rec + EV_OFF_OPP0_NAME),
			e->fromName, sizeof(e->fromName));
		// The movies either side of the race, blob offsets as well.
		readBlobString(str, strSize, (u32)(u16)rd16le(rec + EV_OFF_PRE_MOVIE),
			e->preMovie, sizeof(e->preMovie));
		readBlobString(str, strSize, (u32)(u16)rd16le(rec + EV_OFF_POST_MOVIE),
			e->postMovie, sizeof(e->postMovie));

		// The roster. Entries past opponentCount are left zeroed rather than
		// read: the engine's own loop stops at the count, and the bytes beyond
		// it in a 3-opponent record are another event's business.
		memset(e->opponents, 0, sizeof(e->opponents));
		if (e->opponentCount > EVENTDB_MAX_OPPONENTS)
			e->opponentCount = EVENTDB_MAX_OPPONENTS;
		for (int k = 0; k < (int)e->opponentCount; k++) {
			const u8* o = rec + EV_OFF_OPP0_NAME + k * EV_OPPONENT_SIZE;
			EventOpponent* op = &e->opponents[k];
			readBlobString(str, strSize, (u32)(u16)rd16le(o + EV_OPP_OFF_NAME),
				op->name, sizeof(op->name));
			op->match = (s8)o[EV_OPP_OFF_MATCH];
			op->matchValue = (s8)o[EV_OPP_OFF_MATCHVALUE];
			op->carHash = rd32le(o + EV_OPP_OFF_CAR);
			for (int d = 0; d < 3; d++)
				op->skill[d] = (s8)o[EV_OPP_OFF_SKILL + d];
			op->flag = (s8)o[EV_OPP_OFF_FLAG];
		}

		s8 n = (s8)rec[EV_OFF_TRACKCOUNT];
		if (n > EVENTDB_MAX_EVENT_TRACKS)
			n = EVENTDB_MAX_EVENT_TRACKS;
		if (n < 0)
			n = 0;
		e->trackCount = n;
		for (int k = 0; k < n; k++) {
			const u8* t = rec + EV_OFF_TRACKS + k * 4;
			e->tracks[k].trackId = rd16le(t);
			e->tracks[k].flags = t[2];
			e->tracks[k].extra = t[3];
		}
	}

	out->count = count;
	free(body);
	return count > 0;
}

// SPEED2.EXE:0x514340 -- does this one event unlock the track? The engine takes
// the record pointer the iterator handed it; here it is an index, which is the
// same record. Note the `break`: only the *first* matching entry's flag counts,
// and a non-2 flag falls through to the progress table rather than trying the
// event's other entries.
static bool eventdb_event_passes(const EventRec* e, u16 trackId,
		const EventProgress* prog, int progCount) {
	for (int i = 0; i < e->trackCount; i++) {
		if (e->tracks[i].trackId == (s16)trackId) {
			if ((e->tracks[i].flags & 0x2u) == 0x2u)
				return true;
			break;
		}
	}
	// 0x514340's second loop keys on the event id alone; the type byte at +4
	// that 0x501150 tests is not consulted here, so this does not either.
	for (int i = 0; i < progCount; i++) {
		if (prog[i].eventId == e->eventId)
			return prog[i].state == 1;
	}
	return false;
}

bool eventdb_track_unlocked(const EventDb* db, u16 trackId,
		const EventProgress* prog, int progCount) {
	if (!db)
		return false;
	if (!prog)
		progCount = 0;
	// SPEED2.EXE:0x5143C0 drives SPEED2.EXE:0x4FFCF0, a cursor iterator over
	// the record array that yields every record whose track list mentions the
	// id and skips records with a track count below 1. Walking the array by
	// index yields the same records in the same order, so the cursor is dropped
	// and the "does this record mention the track" test is inlined here.
	for (int i = 0; i < db->count; i++) {
		const EventRec* e = &db->recs[i];
		if (e->trackCount < 1)
			continue;
		bool mentions = false;
		for (int k = 0; k < e->trackCount; k++) {
			if (e->tracks[k].trackId == (s16)trackId) {
				mentions = true;
				break;
			}
		}
		if (!mentions)
			continue;
		if (eventdb_event_passes(e, trackId, prog, progCount))
			return true;
	}
	return false;
}

// FindShopDataDescByTriggerZone (Speed.elf 0x80121F54). The engine walks the
// record array by its 0xA0 stride comparing +0x3C against the handle and
// returns the first match, or null; stepping the parsed array by index visits
// the same records in the same order.
const ShopRec* eventdb_shop_by_trigger(const EventDb* db, u32 triggerHandle) {
	if (!db)
		return NULL;
	for (int i = 0; i < db->shopCount; i++) {
		if (db->shops[i].triggerHandle == triggerHandle)
			return &db->shops[i];
	}
	return NULL;
}

// FindShopDataDescByHandle (Speed.elf 0x80121F14): the same walk against +0x38.
const BankTriggerRec* eventdb_bank_by_zone(const EventDb* db, u32 zoneHash) {
	for (int i = 0; i < db->bankCount; i++)
		if (db->banks[i].zoneHash == zoneHash)
			return &db->banks[i];
	return NULL;
}

const ShopRec* eventdb_shop_by_handle(const EventDb* db, u32 handle) {
	if (!db)
		return NULL;
	for (int i = 0; i < db->shopCount; i++) {
		if (db->shops[i].handle == handle)
			return &db->shops[i];
	}
	return NULL;
}

// FindShowcaseDataByHandle (Speed.elf 0x801224D4) and
// FindShowcaseDataByTriggerZone (0x80122514): the same linear walk, against
// +0x20 and +0x2C.
const ShowcaseRec* eventdb_showcase_by_handle(const EventDb* db, u32 handle) {
	if (!db)
		return NULL;
	for (int i = 0; i < db->showcaseCount; i++) {
		if (db->showcases[i].handle == handle)
			return &db->showcases[i];
	}
	return NULL;
}

const ShowcaseRec* eventdb_showcase_by_trigger(const EventDb* db, u32 handle) {
	if (!db)
		return NULL;
	for (int i = 0; i < db->showcaseCount; i++) {
		if (db->showcases[i].triggerHandle == handle)
			return &db->showcases[i];
	}
	return NULL;
}

// FindWorldChallengeByTriggerZone (Speed.elf 0x80122E1C) hashes
// GetStartAtTriggerName on every record; the hash is taken once at load here.
const WorldChallengeRec* eventdb_challenge_by_trigger(const EventDb* db,
		u32 handle) {
	if (!db)
		return NULL;
	for (int i = 0; i < db->challengeCount; i++) {
		if (db->challenges[i].startHandle == handle)
			return &db->challenges[i];
	}
	return NULL;
}

int eventdb_shop_minimap_element(const ShopRec* shop) {
	// The first switch reads ShopDataDesc+0x50 and answers a trigger zone type
	// (crib 14, paint 9, parts 10, performance 12, car lot 13, ICE 11, and 10
	// for anything else); the minimap's map sends 9..13 to 7..11 and 14 to 6.
	switch (shop ? shop->kind : -1) {
	case 0: return 6;
	case 1: return 7;
	case 3: return 10;
	case 4: return 11;
	case 5: return 9;
	case 2:
	default: return 8;
	}
}

// FindCareerEventDataByHandle: a walk of the table for the id.
u32 eventdb_event_preset_car(const char* bunPath, u32 eventId) {
	long hdrOff = 0;
	u32 size = 0;
	if (!bunidx_find(bunPath, EVENTDB_CONTAINER_TYPE, &hdrOff, &size) || size < 8)
		return 0;
	FILE* f = fopen(bunPath, "rb");
	if (!f)
		return 0;
	u32 car = 0;
	long off = hdrOff + 8;
	const long end = off + (long)size;
	u8 h[8];
	while (off + 8 <= end && fseek(f, off, SEEK_SET) == 0 && fread(h, 1, 8, f) == 8) {
		const u32 st = rd32le(h), ss = rd32le(h + 4);
		if ((st == 0 && ss == 0) || ss > (u32)(end - (off + 8)))
			break;
		if (st == EVENTDB_CHUNK_TYPE) {
			u8 rec[EVENTDB_RECORD_SIZE];
			for (u32 i = 0; i + EVENTDB_RECORD_SIZE <= ss; i += EVENTDB_RECORD_SIZE) {
				if (fread(rec, 1, EVENTDB_RECORD_SIZE, f) != EVENTDB_RECORD_SIZE)
					break;
				if (rd32le(rec + EV_OFF_EVENTID) == eventId) {
					car = rd32le(rec + EV_OFF_PRESET_CAR);
					break;
				}
			}
			break;
		}
		off += 8 + (long)ss;
	}
	fclose(f);
	return car;
}

const EventRec* eventdb_event_by_id(const EventDb* db, u32 eventId) {
	for (int i = 0; db && i < db->count; i++)
		if (db->recs[i].eventId == eventId)
			return &db->recs[i];
	return NULL;
}

// _get_career_event_indicator_type (Speed.elf 0x80111C1C). The engine runs the
// type switch first and keeps its answer in a register, then lets the two tests
// below override it; written here in order of effect, which is the same thing.
// Out-of-range types answer 5, as the engine's own fall-through does.
int eventdb_event_indicator_type(const EventRec* ev) {
	if (!ev)
		return 0;
	if (ev->cls == EVENT_CLASS_URL)
		return 7; // MODE_ICON_U_RACING_LEAGUE
	if (ev->isSUV == 1)
		return 8; // MODE_ICON_SUV
	switch (ev->type) {
	case EVENT_TYPE_SPRINT:
		return 4; // MODE_ICON_SPRINT
	case EVENT_TYPE_DRAG:
		return 2; // MODE_ICON_DRAG
	case EVENT_TYPE_DRIFT:
		return 3; // MODE_ICON_DRIFT
	case EVENT_TYPE_CIRCUIT:
	case EVENT_TYPE_DDAY:
		return 1; // MODE_ICON_CIRCUIT
	case EVENT_TYPE_STREET:
	default:
		return 5; // MODE_ICON_SHORT_TRACK
	}
}

// get_event_trigger_zone_type (0x80123770) composed with the minimap's own
// _map_trigger_zone_type_to_element_type (0x80114E88). Nothing else in this
// port needs the intermediate career trigger-zone type, and both halves are
// small pure switches, so they are written as one.
int eventdb_event_minimap_element(const EventRec* ev) {
	if (!ev)
		return 0;
	int zt;
	if (ev->isSUV == 1) {
		zt = 7;
	} else if (ev->cls == EVENT_CLASS_URL) {
		zt = 5;
	} else {
		switch (ev->type) {
		case EVENT_TYPE_SPRINT:
			zt = 4;
			break;
		case EVENT_TYPE_STREET:
			zt = 2;
			break;
		case EVENT_TYPE_DRAG:
			zt = 1;
			break;
		case EVENT_TYPE_DRIFT:
			zt = 3;
			break;
		case EVENT_TYPE_CIRCUIT:
		case EVENT_TYPE_DDAY:
		default:
			zt = 0;
			break;
		}
	}
	// 0x80114E88 maps 0..5 to 12..17 and 7 to 18. Those are the only arms the
	// composition above can reach.
	return (zt == 7) ? 18 : 12 + zt;
}

// ---------------------------------------------------------------------------
// The engage race dialog's text
// ---------------------------------------------------------------------------

// Setup's defaults: it loads these two before the table runs and lets a later
// arm overwrite them, so a circuit race and a DDAY record both keep them. Both
// resolve in English.bin to "Circuit".
#define ENGAGE_SUBJ_DEFAULT 0x9B39C122u
#define ENGAGE_TYPE_DEFAULT 0x268D063Cu

// Setup's own test for the first table, written out:
// `(cls == REGULAR && !IsSUVRace(ev)) || ev->isSUV == 1`, where IsSUVRace
// (0x803036D4) is `isSUV == 1` and Setup negates it inline (subfic/adde at
// 0x800E879C). Between them the two halves take every ordinary race and every
// SUV race, leaving the URL and sponsor classes to the second table.
static bool engageUsesPlainTable(const EventRec* ev) {
	return (ev->cls == EVENT_CLASS_REGULAR && ev->isSUV != 1) || ev->isSUV == 1;
}

// Setup's table, in one pass because that is how it is written: two registers
// seeded with the defaults, then at most one arm writing them. Several arms
// write only the first -- sponsored circuit is the clearest -- so the second
// keeps "Circuit" there, and that asymmetry is the reason this is one function
// with two outputs rather than two independent switches.
static void engageTitleIds(const EventRec* ev, u32* subject, u32* type) {
	*subject = ENGAGE_SUBJ_DEFAULT;
	*type = ENGAGE_TYPE_DEFAULT;
	if (!ev)
		return;
	if (engageUsesPlainTable(ev)) {
		switch (ev->type) {
		case EVENT_TYPE_SPRINT: *subject = 0x9EC5F82Fu; *type = 0xBA613729u; break;
		case EVENT_TYPE_STREET: *subject = 0x57F833ADu; *type = 0x0FDA6758u; break;
		case EVENT_TYPE_DRAG:   *subject = 0xE45A7387u; *type = 0x40C91BA7u; break;
		case EVENT_TYPE_DRIFT:  *subject = 0x3A10A7C2u; *type = 0x59ECB2C2u; break;
		default: break; // circuit, and DDAY, keep both defaults
		}
		return;
	}
	if (ev->cls == EVENT_CLASS_URL) {
		*subject = 0x80B8267Cu; // "URL"
		*type = 0xB46377BCu;    // "U.R.L."
		return;
	}
	if (ev->cls != EVENT_CLASS_SPON)
		return;
	switch (ev->type) {
	// Sponsored circuit writes the subject only, so the line under it reads
	// "Circuit" rather than naming the sponsor family. Retail's own wording.
	case EVENT_TYPE_CIRCUIT: *subject = 0x0F037A7Bu; break;
	case EVENT_TYPE_SPRINT:  *subject = 0x07216A68u; *type = 0xBA613729u; break;
	case EVENT_TYPE_STREET:  *subject = 0xC5F7902Cu; *type = 0x0FDA6758u; break;
	case EVENT_TYPE_DRAG:    *subject = 0x3D6A70E6u; *type = 0x40C91BA7u; break;
	case EVENT_TYPE_DRIFT:   *subject = 0xB5205101u; *type = 0x59ECB2C2u; break;
	default: break;
	}
}

u32 eventdb_engage_subject_id(const EventRec* ev) {
	u32 subject, type;
	engageTitleIds(ev, &subject, &type);
	(void)type;
	return subject;
}

u32 eventdb_engage_type_id(const EventRec* ev) {
	u32 subject, type;
	engageTitleIds(ev, &subject, &type);
	(void)subject;
	return type;
}

u32 eventdb_event_purse(const EventDb* db, const EventRec* ev, u32 chosenSponsor) {
	if (!ev)
		return 0;
	if (ev->cls != EVENT_CLASS_SPON)
		return ev->purse;
	const SponsorRec* s = eventdb_sponsor_by_hash(db, chosenSponsor);
	// The engine's own null arm: no sponsor chosen, so the event's purse stands.
	return s ? s->purse : ev->purse;
}

const SponsorRec* eventdb_sponsor_by_hash(const EventDb* db, u32 nameHash) {
	if (!db || !nameHash)
		return NULL;
	for (int i = 0; i < db->sponsorCount; i++)
		if (db->sponsors[i].nameHash == nameHash)
			return &db->sponsors[i];
	return NULL;
}

const StageRec* eventdb_stage(const EventDb* db, int stage) {
	if (!db)
		return NULL;
	// Keyed on the record's own stage byte, not an index, so a table that is
	// out of order or short still answers correctly.
	for (int i = 0; i < db->stageCount; i++)
		if (db->stages[i].stage == (s8)stage)
			return &db->stages[i];
	return NULL;
}

u32 eventdb_url_purse(const EventDb* db, int stage) {
	if (!db)
		return 0;
	for (int i = 0; i < db->count; i++) {
		const EventRec* e = &db->recs[i];
		if (e->stage == (s8)stage && e->cls == EVENT_CLASS_URL)
			return e->purse;
	}
	return 0;
}

// Uppercase-folded bStringHash, the key the body label is looked up by
// (SPEED2.EXE:0x50B9C0): seed 0xFFFFFFFF, h = c + h * 0x21. The labels built
// below are an uppercase literal plus a decimal, and the shipped event names
// are uppercase too, so the fold has nothing to do here -- it is kept because
// FEngHashString's own does it.
static u32 engageLabelHash(const char* str) {
	u32 h = 0xFFFFFFFFu;
	for (const unsigned char* p = (const unsigned char*)str; *p; p++) {
		unsigned char c = *p;
		if (c >= 'a' && c <= 'z')
			c = (unsigned char)(c - 0x20);
		h = (u32)c + h * 0x21u;
	}
	return h;
}

// The body family and how many written variants it has, both out of
// GetEngageDialogBody (0x80121B88): it loads a format string and a count into
// two registers and ends on FEngHashString(fmt, bRandom(count) + 1).
static const char* engageBodyFamily(const EventRec* ev, int* variants) {
	*variants = 0;
	if (ev->isSUV == 1) {
		*variants = 9;
		return "ENGAGE_SUV_RANDOM_%d";
	}
	if (ev->cls == EVENT_CLASS_SPON) {
		switch (ev->type) {
		case EVENT_TYPE_CIRCUIT: *variants = 11; return "ENGAGE_SPON_CIRCUIT_RANDOM_%d";
		case EVENT_TYPE_SPRINT:  *variants = 10; return "ENGAGE_SPON_SPRINT_RANDOM_%d";
		case EVENT_TYPE_STREET:  *variants = 10; return "ENGAGE_SPON_STREET_RANDOM_%d";
		case EVENT_TYPE_DRAG:    *variants =  8; return "ENGAGE_SPON_DRAG_RANDOM_%d";
		case EVENT_TYPE_DRIFT:   *variants = 11; return "ENGAGE_SPON_DRIFT_RANDOM_%d";
		default:                 return NULL;
		}
	}
	if (ev->cls == EVENT_CLASS_URL) {
		*variants = 12;
		return "ENGAGE_URL_RANDOM_%d";
	}
	switch (ev->type) {
	case EVENT_TYPE_CIRCUIT: *variants = 18; return "ENGAGE_CIRCUIT_RANDOM_%d";
	case EVENT_TYPE_SPRINT:  *variants = 15; return "ENGAGE_SPRINT_RANDOM_%d";
	case EVENT_TYPE_STREET:  *variants = 14; return "ENGAGE_STREET_RANDOM_%d";
	case EVENT_TYPE_DRAG:    *variants = 12; return "ENGAGE_DRAG_RANDOM_%d";
	case EVENT_TYPE_DRIFT:   *variants = 13; return "ENGAGE_DRIFT_RANDOM_%d";
	// Type 3 (DDAY) is retail's latent bug: the engine sets a count of 10 here
	// and leaves the format register at the 0 it was initialised with, so the
	// call would hash a NULL. Unreachable in practice -- both DDAY records
	// carry class 0, which never reaches this arm. Left empty, not repaired.
	default:                 return NULL;
	}
}

int eventdb_engage_body_variants(const EventRec* ev) {
	if (!ev)
		return 0;
	// A stage-0 record has one body of its own rather than a random family.
	if (ev->stage == 0)
		return ev->name[0] ? 1 : 0;
	int n = 0;
	const char* fmt = engageBodyFamily(ev, &n);
	return fmt ? n : 0;
}

u32 eventdb_engage_body_label(const EventRec* ev, int variant,
		char* out, size_t outSize) {
	if (out && outSize)
		out[0] = '\0';
	if (!ev)
		return 0;

	char label[64];
	if (ev->stage == 0) {
		if (!ev->name[0])
			return 0;
		snprintf(label, sizeof(label), "ENGAGE_%s", ev->name);
	} else {
		int n = 0;
		const char* fmt = engageBodyFamily(ev, &n);
		if (!fmt || n <= 0)
			return 0;
		if (variant < 1)
			variant = 1;
		if (variant > n)
			variant = n;
		snprintf(label, sizeof(label), fmt, variant);
	}

	if (out && outSize) {
		strncpy(out, label, outSize - 1);
		out[outSize - 1] = '\0';
	}
	return engageLabelHash(label);
}

// FEngHashString (Speed.elf 0x8014B6C4): the same djb2 bStringHash every
// other name in this port goes through, over the upper-cased string. The
// fold is the engine's, not an accommodation -- it is why the mailbox's
// authored `Body_Text_1` and `BODY_TEXT_1` name the same widget.
u32 eventdb_feng_hash(const char* s) {
	u32 h = 0xFFFFFFFFu;
	for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
		unsigned char c = *p;
		if (c >= 'a' && c <= 'z')
			c = (unsigned char)(c - 'a' + 'A');
		h = h * 33u + c;
	}
	return h;
}

int eventdb_sms_find(const EventDb* db, u32 nameHash) {
	if (!db || nameHash == 0)
		return -1;
	for (int i = 0; i < db->smsCount; i++) {
		if (db->sms[i].nameHash == nameHash)
			return i;
	}
	return -1;
}

float eventdb_stage_ai_perf(const StageRec* s, int racesWon) {
	if (!s)
		return 0.0f;
	// 0x80124598: `done > total || total == 0` takes the top; otherwise the
	// share of the way from lo to hi.
	if (racesWon > s->aiRaces || s->aiRaces == 0)
		return s->aiPerfHi;
	return (float)racesWon / (float)s->aiRaces * (s->aiPerfHi - s->aiPerfLo) + s->aiPerfLo;
}
