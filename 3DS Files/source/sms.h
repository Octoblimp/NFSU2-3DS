#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "eventdb.h"
#include "profile_save.h"

// The Cingular mailbox's model: which of the save's queued messages a tab
// shows, in what order, and which language label each one's subject and body
// come from.
//
// Everything here is SMSMailboxViewer's and SMSMessage's, read out of
// Speed.elf. The drawing is main.c's, because the .fng screens
// (UI_SMS_Mailbox, UI_EngageMessageDialog) and the language table live there;
// this file decides *what* goes in each row, never how it looks.
//
// The split shows in the return type: a label is handed back as an
// SmsTextRef -- the hashes to look up -- rather than as text, so the one
// langbin_lookup/langTextToUtf8 path in main.c stays the only one.

// SMSMessageMailboxCategories, the tabs. The order is
// GetMailboxCategoryLanguageHash's (Speed.elf 0x800DC84C), which switches on
// 0..5 in this order and returns 0 for anything else. SMS_CAT_*_ID in
// eventdb.h are those six label hashes.
typedef enum {
	SMS_MAILBOX_MISC = 0,
	SMS_MAILBOX_INBOX = 1,
	SMS_MAILBOX_TIPS = 2,
	SMS_MAILBOX_EVENTS = 3,
	SMS_MAILBOX_RACHEL = 4,
	SMS_MAILBOX_UNLOCKS = 5
} SmsMailboxCategory;

// What one call to GetSubject (0x8012BA10) or GetBody (0x8012BB04) resolves
// to. Both have the same shape: a parametrised form when the queue slot
// carries a frame hash, and a plain label otherwise.
//
//   frameId != 0 -> the text is GetLocalizedString(frameId) with its one %s
//                   replaced by GetLocalizedString(argId).
//   frameId == 0 -> the text is GetLocalizedString(labelId), where labelId is
//                   the hash of a label built from the message's name.
typedef struct {
	u32 frameId;
	u32 argId;
	u32 labelId;
} SmsTextRef;

// One tab's messages, newest first.
//
// BuildSubjectHeaderList (0x800DCEA4) walks the queue backwards -- from
// count-1 down to 0 -- adding every message CanDisplayMessageInCurrentList
// accepts, so the message added last is the first row. `idx` holds indices
// into Profile.sms[], which is the queue in its own order.
#define SMS_VIEW_MAX PROFILE_MAX_SMS

typedef struct {
	int category;
	int count;
	unsigned short idx[SMS_VIEW_MAX];
} SmsView;

// CanDisplayMessageInCurrentList (0x800DD1DC): deleted messages show nowhere;
// a message that has not been sorted yet shows only in the Inbox; once sorted
// it shows only in its own category.
bool sms_can_display(const EventDb* db, const ProfileSmsSlot* m, int category);

// The rows of one tab, in the order they are drawn.
void sms_build_view(const Profile* p, const EventDb* db, int category, SmsView* out);

// The tab's own heading, GetMailboxCategoryLanguageHash's return.
u32 sms_category_label_id(int category);

// GetSubject / GetBody, up to the lookup. `db` may be NULL, in which case
// only the frame path can resolve -- a name is needed for the label path.
void sms_subject_ref(const EventDb* db, const ProfileSmsSlot* m, SmsTextRef* out);
void sms_body_ref(const EventDb* db, const ProfileSmsSlot* m, SmsTextRef* out);

// GetFrom (0x8012BA04): the sender, straight off the message's record.
u32 sms_from_id(const EventDb* db, const ProfileSmsSlot* m);

// SMSGetEnvelopeImageHashAndColour (0x800DC308), the colour half: what
// SyncCurrentButton (0x800DD0CC) tints the row's envelope with, as 0xRRGGBBAA.
// The texture half is not this function's -- SyncCurrentButton throws the
// returned hash away and picks on the read flag instead, which is what
// SMS_ENVELOPE_READ_TEX / SMS_ENVELOPE_NEW_TEX are.
#define SMS_ENVELOPE_READ_TEX 0xEE28AC57u
#define SMS_ENVELOPE_NEW_TEX  0x4B9B8F5Au
u32 sms_envelope_color(const EventDb* db, const ProfileSmsSlot* m);

// NotifyReadMessage (0x8012C588) and NotifyDeleteMessage (0x8012C610). Both
// set their byte on the slot and leave the queue's order alone; the caller
// writes the save back.
void sms_mark_read(Profile* p, int slotIndex);
void sms_mark_deleted(Profile* p, int slotIndex);

// MarkAllReadMessagesSortable (0x8012C3F4): every queued message already read
// gets its sortable byte, which is what moves it out of the Inbox into its
// own category (sms_can_display()). Retail runs it whenever the SMS viewer
// goes back to driving (BackToExplore 0x800DC598) and on the reader's back
// out to the mailbox it came from (SMSMessageViewer::NotificationMessage,
// PAD_BACK). Returns how many bytes it changed; the caller writes the save.
int sms_mark_all_read_sortable(Profile* p);

// The mailbox category the message's record carries (SmsRec +0x03), -1 when
// its record is not in the table. The reader's back to the mailbox opens on
// this tab.
int sms_record_category(const EventDb* db, const ProfileSmsSlot* m);

// AddExternalMessage (0x8012BD30) into AddMessage (0x8012C000): queues the
// career table's message `nameHash` (a bStringHash of its name) unread in
// the next free slot. Returns its index in p->sms[], or -1 when the table has
// no such message, it is queued already, or the 256 slots are spent.
// AddMessage's CareerSpeech::HandleSMS is empty in retail (SpeedPS.elf
// 0x80189140); the prologue plays its own line for SMS_INSTRUCTION (Stage0SMS).
int sms_add_external_message(Profile* p, const EventDb* db, u32 nameHash);

// AddMagazineMessage (0x8012C098): the same queueing, then MakeMagazineMessage
// (0x8012B9F4) stores the body frame (+0x08), the argument (+0x0C) and the
// subject frame (+0x10) on the slot, which is how GetBody / GetSubject come to
// read a frame for it rather than a label built from its name. The world
// challenges' SMS_MAG_* and SMS_UNIQUE_* messages are raised this way
// (AddWorldChallengeMessage, 0x8012BD88). Returns the slot or -1, as above.
int sms_add_magazine_message(Profile* p, const EventDb* db, u32 nameHash,
	u32 bodyFrameId, u32 subjectFrameId, u32 argId);

// NotifyCompleteEvent (0x8012C6D4), which CommitWinEvent calls on every win:
// each message IsTriggeredByCompletingEvent holds for and not queued yet goes
// through AddMessage, in table order. Returns how many were queued.
int sms_notify_complete_event(Profile* p, const EventDb* db, u32 eventId);

// NotifyDVDCoverElegible (SpeedPS.elf 0x8012C754), which RecalcDVDUnlocks
// calls as a DVD cover's star threshold is first reached: each message
// IsTriggeredByHavingDVDCover (0x8012B91C: trigger 14, +0x08 == `stars`, the
// threshold in whole stars) holds for and not queued yet goes through
// AddMessage. Returns how many were queued.
int sms_notify_dvd_cover_eligible(Profile* p, const EventDb* db, u32 stars);

// The getaway's notifiers (#268 H step 2): NotifyBeginGetAway (0x8012C87C)
// queues every trigger 4 message, NotifyFinishGetaway (0x8012C8F4) every
// trigger 5 one on a win or trigger 6 one on a loss -- each not queued yet,
// through AddMessage, in table order. Returns how many were queued.
int sms_notify_trigger(Profile* p, const EventDb* db, int trigger);

// The SMS auto-trigger zones (#344): TrackPathZone type 0x12 (TPZ_SMS_TRIGGER),
// 56 of them in PathsFreeRoam.bin, each naming one of the 42 trigger 15
// records -- the ENGAGE_TIPS_* game tips, $25 or $50 each -- by its +0x30,
// which the record carries at +0x08.
#define SMS_TRIGGER_AUTO_ZONE 15

// SMSMessageQueue::NotifyInSMSAutoTriggerZone (SpeedPS.elf 0x8012C9A8): the
// first trigger 15 record naming the zone; nothing if it is queued already,
// else AddMessage. Returns the slot, or -1, and on a queue `*cash` gets the
// record's +0x0C, which PlayerCareerState::NotifyInSMSAutoTriggerZone
// (0x801260BC) deposits before RecalcHelpTriggers.
int sms_notify_auto_trigger_zone(Profile* p, const EventDb* db, u32 zoneParam, u32* cash);

// CanHelpTriggerZoneBeActivated (0x8012CA54): the zone names a trigger 15
// record that is not queued yet. RecalcHelpTriggers' (0x80128F3C) test.
bool sms_help_zone_can_activate(const Profile* p, const EventDb* db, u32 zoneParam);

// SMSMessageQueue::Tick's first half (SPEED2.EXE 0x529750, SpeedPS.elf
// 0x8012C488): each trigger 3 message whose +0x08, in seconds, the career
// clock has passed and not queued yet goes through AddMessage. Returns how
// many were queued.
int sms_notify_career_time(Profile* p, const EventDb* db, float careerTime);

// AnyUnseenAutoPopups (0x8012BC38): a queued message whose record pops up by
// itself (+0x04 == 1) and that has not popped up yet (slot +0x05 == 0).
bool sms_any_unseen_autopopups(const Profile* p, const EventDb* db);

// MarkAllAutoPopupsSeen (0x8012BCC4): popped set on every queued message whose
// record pops up by itself.
void sms_mark_all_autopopups_seen(Profile* p, const EventDb* db);

// GetNumMessagesUnread (0x8012C36C) with its argument false: queued, not
// deleted, not read.
unsigned int sms_count_unread(const Profile* p);
