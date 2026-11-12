#include "sms.h"

#include <stdio.h>
#include <string.h>

// bStrNCmp(name, prefix, bStrLen(prefix) - 1) == 0, which is how both
// GetSubject (Speed.elf 0x8012BA10) and GetBody (0x8012BB04) test a name --
// the length is computed from the prefix and then one is taken off it, so
// "SMS_CAR_UNLOCK_" matches on its first fourteen characters and
// "ENGAGE_TIPS_AUTO" on its first fifteen. The short count is the engine's,
// kept because it is what decides the families: ENGAGE_TIPS_AUDIO, which does
// have its own label, differs from ENGAGE_TIPS_AUTO at the fifteenth
// character and so is still excluded.
static bool retailPrefix(const char* name, const char* prefix) {
	size_t n = strlen(prefix);
	if (n == 0)
		return true;
	return strncmp(name, prefix, n - 1) == 0;
}

static const SmsRec* smsRec(const EventDb* db, const ProfileSmsSlot* m) {
	if (!db || !m)
		return NULL;
	int i = eventdb_sms_find(db, m->id);
	return i >= 0 ? &db->sms[i] : NULL;
}

bool sms_can_display(const EventDb* db, const ProfileSmsSlot* m, int category) {
	// CanDisplayMessageInCurrentList (0x800DD1DC), in its own order: the
	// deleted byte first, then the sortable one deciding which of the two
	// tests the rest of the function is.
	if (!m || m->deleted == 1)
		return false;
	if (m->sortable == 0) {
		// Not sorted yet, so it sits in the Inbox and nowhere else --
		// whatever category its record carries.
		return category == SMS_MAILBOX_INBOX;
	}
	const SmsRec* rec = smsRec(db, m);
	if (!rec)
		return false;
	return (int)rec->category == category;
}

void sms_build_view(const Profile* p, const EventDb* db, int category, SmsView* out) {
	if (!out)
		return;
	out->category = category;
	out->count = 0;
	if (!p)
		return;
	// BuildSubjectHeaderList (0x800DCEA4) counts down from the last queued
	// message to the first, so the newest accepted one is row 1.
	for (int i = (int)p->smsCount - 1; i >= 0; i--) {
		if (i >= PROFILE_MAX_SMS)
			continue;
		if (!sms_can_display(db, &p->sms[i], category))
			continue;
		if (out->count >= SMS_VIEW_MAX)
			break;
		out->idx[out->count++] = (unsigned short)i;
	}
}

u32 sms_category_label_id(int category) {
	switch (category) {
		case SMS_MAILBOX_MISC:    return SMS_CAT_MISC_ID;
		case SMS_MAILBOX_INBOX:   return SMS_CAT_INBOX_ID;
		case SMS_MAILBOX_TIPS:    return SMS_CAT_TIPS_ID;
		case SMS_MAILBOX_EVENTS:  return SMS_CAT_EVENTS_ID;
		case SMS_MAILBOX_RACHEL:  return SMS_CAT_RACHEL_ID;
		case SMS_MAILBOX_UNLOCKS: return SMS_CAT_UNLOCKS_ID;
		default:                  return 0;
	}
}

void sms_subject_ref(const EventDb* db, const ProfileSmsSlot* m, SmsTextRef* out) {
	if (!out)
		return;
	out->frameId = 0;
	out->argId = 0;
	out->labelId = 0;
	if (!m)
		return;
	// The parametrised form, which only a message raised through
	// MakeMagazineMessage (0x8012B9F4) carries.
	if (m->subjectFrameId != 0) {
		out->frameId = m->subjectFrameId;
		out->argId = m->argId;
		return;
	}
	const SmsRec* rec = smsRec(db, m);
	if (!rec)
		return;
	if (retailPrefix(rec->name, "SMS_CAR_UNLOCK_")) {
		// Every car unlock shares one subject, however many of them the
		// table names.
		out->labelId = eventdb_feng_hash("SMS_CAR_UNLOCK_1_SUBJECT");
		return;
	}
	char label[EVENTDB_MAX_NAME + 16];
	snprintf(label, sizeof(label), "%s_SUBJECT", rec->name);
	out->labelId = eventdb_feng_hash(label);
}

void sms_body_ref(const EventDb* db, const ProfileSmsSlot* m, SmsTextRef* out) {
	if (!out)
		return;
	out->frameId = 0;
	out->argId = 0;
	out->labelId = 0;
	if (!m)
		return;
	if (m->bodyFrameId != 0) {
		out->frameId = m->bodyFrameId;
		out->argId = m->argId;
		return;
	}
	const SmsRec* rec = smsRec(db, m);
	if (!rec)
		return;
	if (retailPrefix(rec->name, "SMS_CAR_UNLOCK_")) {
		out->labelId = eventdb_feng_hash("SMS_CAR_UNLOCK_1_BODY");
		return;
	}
	if (retailPrefix(rec->name, "ENGAGE_TIPS_AUTO")) {
		// The whole auto-tip family reads one body; which tip it is comes
		// from the record's own arg0, not from the name.
		out->labelId = eventdb_feng_hash("ENGAGE_TIPS_AUTO_BODY");
		return;
	}
	char label[EVENTDB_MAX_NAME + 16];
	snprintf(label, sizeof(label), "%s_BODY", rec->name);
	out->labelId = eventdb_feng_hash(label);
}

u32 sms_from_id(const EventDb* db, const ProfileSmsSlot* m) {
	const SmsRec* rec = smsRec(db, m);
	return rec ? rec->fromId : 0;
}

void sms_mark_read(Profile* p, int slotIndex) {
	if (!p || slotIndex < 0 || slotIndex >= (int)p->smsCount || slotIndex >= PROFILE_MAX_SMS)
		return;
	ProfileSmsSlot* m = &p->sms[slotIndex];
	// NotifyReadMessage (0x8012C588) does nothing to a message already read,
	// which is what keeps the unread count from going under.
	if (m->read == 1)
		return;
	m->read = 1;
	if (p->smsUnread > 0)
		p->smsUnread--;
}

int sms_add_external_message(Profile* p, const EventDb* db, u32 nameHash) {
	// AddExternalMessage (0x8012BD30): GetSMSMessageDataByNameHash, and
	// nothing at all for a name the career table does not carry or a message
	// already queued -- IsMessageQueued (0x8012C150) walks every slot handed
	// out, read and deleted ones too.
	if (!p || eventdb_sms_find(db, nameHash) < 0)
		return -1;
	unsigned int next = 0;
	for (unsigned int i = 0; i < p->smsCount && i < PROFILE_MAX_SMS; i++) {
		if (p->sms[i].id == nameHash)
			return -1;
		if (p->sms[i].slot + 1u > next)
			next = p->sms[i].slot + 1u;
	}
	// AddMessage (0x8012C000): AllocateMessage (0x8012BFD8) hands out the
	// next of the 256 slots -- the queue never gives one back -- and BuildFrom
	// (0x8012B9C8) zeroes every byte and frame word but the message.
	if (p->smsCount >= PROFILE_MAX_SMS || next >= PROFILE_MAX_SMS)
		return -1;
	ProfileSmsSlot* m = &p->sms[p->smsCount];
	memset(m, 0, sizeof(*m));
	m->slot = (unsigned short)next;
	m->id = nameHash;
	p->smsCount++;
	p->smsUnread++;
	return (int)p->smsCount - 1;
}

int sms_add_magazine_message(Profile* p, const EventDb* db, u32 nameHash,
		u32 bodyFrameId, u32 subjectFrameId, u32 argId) {
	const int k = sms_add_external_message(p, db, nameHash);
	if (k < 0)
		return -1;
	p->sms[k].bodyFrameId = bodyFrameId;
	p->sms[k].argId = argId;
	p->sms[k].subjectFrameId = subjectFrameId;
	return k;
}

// The non-zero slots of a stage record's list: GetNumWorldRacesCompleted
// (0x80124674), GetNumURLRacesCompleted (0x801246A0) and
// GetNumSponsorRacesCompleted (0x801246D0).
static int smsRacesWon(const unsigned int* list, int n) {
	int won = 0;
	for (int i = 0; i < n; i++)
		if (list[i])
			won++;
	return won;
}

// IsTriggeredByCompletingEvent (0x8012B7FC). Trigger 1 names the event in
// either argument word. 9, 10 and 12 ignore the event: the first argument's
// high half is a stage and its low half a count, and the message is due once
// that stage has won that many sponsor (9), URL (10) or world (12) races.
static bool smsTriggeredByCompletingEvent(const Profile* p, const SmsRec* r, u32 eventId) {
	if (r->trigger == 1 && (r->arg0 == eventId || r->arg1 == eventId))
		return true;
	if (r->trigger != 9 && r->trigger != 10 && r->trigger != 12)
		return false;
	const unsigned int stage = r->arg0 >> 16;
	if (stage >= PROFILE_NUM_STAGES)
		return false;
	const ProfileStageInfo* si = &p->stage[stage];
	const int won = r->trigger == 9 ? smsRacesWon(si->sponsorRace, PROFILE_STAGE_SPONSOR_RACES)
		: r->trigger == 10 ? smsRacesWon(si->urlRace, PROFILE_STAGE_URL_RACES)
		: smsRacesWon(si->worldRace, PROFILE_STAGE_WORLD_RACES);
	return (int)(r->arg0 & 0xFFFF) <= won;
}

int sms_notify_complete_event(Profile* p, const EventDb* db, u32 eventId) {
	if (!p || !db)
		return 0;
	int n = 0;
	for (int i = 0; i < db->smsCount && i < EVENTDB_MAX_SMS; i++)
		if (smsTriggeredByCompletingEvent(p, &db->sms[i], eventId) &&
				sms_add_external_message(p, db, db->sms[i].nameHash) >= 0)
			n++;
	return n;
}

int sms_notify_dvd_cover_eligible(Profile* p, const EventDb* db, u32 stars) {
	if (!p || !db)
		return 0;
	int n = 0;
	for (int i = 0; i < db->smsCount && i < EVENTDB_MAX_SMS; i++)
		if (db->sms[i].trigger == 14 && db->sms[i].arg0 == stars &&
				sms_add_external_message(p, db, db->sms[i].nameHash) >= 0)
			n++;
	return n;
}

int sms_notify_trigger(Profile* p, const EventDb* db, int trigger) {
	if (!p || !db)
		return 0;
	int n = 0;
	for (int i = 0; i < db->smsCount && i < EVENTDB_MAX_SMS; i++)
		if (db->sms[i].trigger == trigger &&
				sms_add_external_message(p, db, db->sms[i].nameHash) >= 0)
			n++;
	return n;
}

// The first trigger 15 record whose +0x08 is the zone's +0x30, the search
// NotifyInSMSAutoTriggerZone and CanHelpTriggerZoneBeActivated both make:
// GetSMSMessageDataByIndex in table order, stopping at the first match.
static int smsAutoTriggerRec(const EventDb* db, u32 zoneParam) {
	for (int i = 0; db && i < db->smsCount && i < EVENTDB_MAX_SMS; i++)
		if (db->sms[i].trigger == SMS_TRIGGER_AUTO_ZONE && db->sms[i].arg0 == zoneParam)
			return i;
	return -1;
}

static bool smsQueued(const Profile* p, u32 nameHash) {
	for (unsigned int i = 0; i < p->smsCount && i < PROFILE_MAX_SMS; i++)
		if (p->sms[i].id == nameHash)
			return true;
	return false;
}

int sms_notify_auto_trigger_zone(Profile* p, const EventDb* db, u32 zoneParam, u32* cash) {
	if (!p)
		return -1;
	const int r = smsAutoTriggerRec(db, zoneParam);
	if (r < 0 || smsQueued(p, db->sms[r].nameHash))
		return -1;
	const int k = sms_add_external_message(p, db, db->sms[r].nameHash);
	if (k >= 0 && cash)
		*cash = db->sms[r].arg1;
	return k;
}

bool sms_help_zone_can_activate(const Profile* p, const EventDb* db, u32 zoneParam) {
	const int r = smsAutoTriggerRec(db, zoneParam);
	return p && r >= 0 && !smsQueued(p, db->sms[r].nameHash);
}

int sms_notify_career_time(Profile* p, const EventDb* db, float careerTime) {
	if (!p || !db)
		return 0;
	int n = 0;
	// The argument is a u32 the engine converts to float before it compares.
	for (int i = 0; i < db->smsCount && i < EVENTDB_MAX_SMS; i++)
		if (db->sms[i].trigger == 3 && (float)db->sms[i].arg0 < careerTime &&
				sms_add_external_message(p, db, db->sms[i].nameHash) >= 0)
			n++;
	return n;
}

bool sms_any_unseen_autopopups(const Profile* p, const EventDb* db) {
	if (!p)
		return false;
	for (unsigned int i = 0; i < p->smsCount && i < PROFILE_MAX_SMS; i++) {
		const SmsRec* r = smsRec(db, &p->sms[i]);
		if (r && r->autoPopup == 1 && p->sms[i].popped == 0)
			return true;
	}
	return false;
}

void sms_mark_all_autopopups_seen(Profile* p, const EventDb* db) {
	if (!p)
		return;
	for (unsigned int i = 0; i < p->smsCount && i < PROFILE_MAX_SMS; i++) {
		const SmsRec* r = smsRec(db, &p->sms[i]);
		if (r && r->autoPopup == 1)
			p->sms[i].popped = 1;
	}
}

void sms_mark_deleted(Profile* p, int slotIndex) {
	if (!p || slotIndex < 0 || slotIndex >= (int)p->smsCount || slotIndex >= PROFILE_MAX_SMS)
		return;
	ProfileSmsSlot* m = &p->sms[slotIndex];
	// NotifyDeleteMessage (0x8012C610) takes one off the unread count only
	// when the message was neither deleted nor read already, then sets both
	// bytes -- a deleted message counts as read.
	const bool wasCounted = (m->deleted != 1 && m->read != 1);
	m->deleted = 1;
	m->read = 1;
	if (wasCounted && p->smsUnread > 0)
		p->smsUnread--;
}

int sms_mark_all_read_sortable(Profile* p) {
	if (!p)
		return 0;
	// MarkAllReadMessagesSortable (0x8012C3F4): byte 4 == 1 -> byte 7 = 1,
	// over the whole queue, deleted messages included.
	int changed = 0;
	for (unsigned int i = 0; i < p->smsCount && i < PROFILE_MAX_SMS; i++) {
		ProfileSmsSlot* m = &p->sms[i];
		if (m->read == 1 && m->sortable != 1) {
			m->sortable = 1;
			changed++;
		}
	}
	return changed;
}

int sms_record_category(const EventDb* db, const ProfileSmsSlot* m) {
	const SmsRec* rec = smsRec(db, m);
	return rec ? (int)rec->category : -1;
}

// The eight arms of SMSGetEnvelopeImageHashAndColour's switch, in order,
// each one an FEColor built on the stack and assigned over the caller's. The
// engine stores an FEColor B,G,R,A (see fng.h); these are written the way they
// are read on screen.
u32 sms_envelope_color(const EventDb* db, const ProfileSmsSlot* m)
{
	static const u32 kColor[8] = {
		0xC8C82DFFu, // 1
		0xD5802BFFu, // 2
		0xCAF48AFFu, // 3
		0xCF3030FFu, // 4
		0x8080FFFFu, // 5
		0x3282C8FFu, // 6
		0x804AB5FFu, // 7
		0x0E9C0EFFu  // anything else, the arm the shipped table mostly takes
	};
	const SmsRec* rec = smsRec(db, m);
	const int c = rec ? (int)rec->envelope : 0;
	return (c >= 1 && c <= 7) ? kColor[c - 1] : kColor[7];
}

unsigned int sms_count_unread(const Profile* p) {
	if (!p)
		return 0;
	unsigned int n = 0;
	for (unsigned int i = 0; i < p->smsCount && i < PROFILE_MAX_SMS; i++) {
		if (p->sms[i].read == 0 && p->sms[i].deleted != 1)
			n++;
	}
	return n;
}
