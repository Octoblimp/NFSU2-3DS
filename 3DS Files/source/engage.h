#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

// Retail's Engage widget -- the rounded panel with the D-pad rosette that sits
// in the top-left corner of the in-game HUD (CONTINUE.MD 1a #16, TASKS.md #5).
//
// This is HUD_SingleRace.fng's group 904267 (InGameFreeRoam.bun), the group the
// GameCube build calls EventIndicator, drawn with its own art at its own
// authored positions. It is not a Cingular wordmark: the wordmark
// (HUD_Cingular_Logo) is referenced only by UI_SMS_Mailbox.fng and the three
// UI_Engage*Dialog.fng screens and never by the HUD.
//
// Where every number comes from
// -----------------------------
// EventIndicator's constructor (Speed.elf 0x801117C0) registers the group's
// widgets by object hash, and those hashes are exactly the guids below:
//
//   +0x20  0x50910822  3rdperson_DPAD_PS2.tga    guid 904700  the rosette centre
//   +0x28  0xA206A0B4  Engage_DPAD_Highlight.tga guid 902460  the right arrow
//   +0x2C  0x0A729B1B  Engage_DPAD_Highlight.tga guid 902463  the up arrow
//   +0x34  0x3B1B624A  Mode_Icon_Circuit.tga     guid 900478  the tab
//   +0x38  0x4B9B8F5A  Engage_SMS_mail.tga       guid 919770  the envelope
//   +0x3C  0x668C9CD6  (FEString)                guid 923595  the count line
//          0x99F665F4  Engage_Backing.tga        guid 903853  the panel
//          0xB996A84C  Engage_DPAD_Backing.tga   guid 903864  the rosette
//
// Canvas positions are those widgets' own, summed down the PA parent chain from
// group 904267 at (-247.125, -121.625); tints and alphas are their SA blobs.
//
// What it is for
// --------------
// Each lit arrow is one direction of the D-pad, and it points at the thing that
// direction opens. Only two of the four are ever lit, and the two Update paths
// pick them:
//
//   EngageEvent::UpdateGeneric (0x80110E0C) shows the tab (Mode_Icon) and the
//   UP arrow directly under it, and puts the event's own icon in the tab --
//   GetEventTypeTexHash (0x801115F0) maps EventIndicatorType to Mode_Icon_Circuit,
//   _Drag, _Drift, _Sprint, _Short_Track, _U_Racing_League, _SUV,
//   _World_Challenge, Icon_Random_Encounter, Icon_Showcase and the six
//   Shop_Icon_* textures.
//
//   EngageEvent::UpdateSMS (0x80111014) shows the envelope to the right of the
//   rosette and the RIGHT arrow between the two.
//
// Player::RacingJoyHandler (0x8021A930) is the other half, and it agrees: the
// four consecutive JoyEvents 0xBF..0xC2 are the D-pad, in this codebase's own
// Up/Down/Left/Right order (MapJoyEventToFEPad, 0x8035D874, orders JoyEvents
// 42..45 as FEPad_Up, _Down, _Left, _Right):
//
//   0xBF  Up     RCMessage 0x11 -> SummonTheRightEngageDialog (0x800E6CEC),
//                guarded by PlayerCareerState::CanWeEngage; with nothing to
//                engage it falls through to the random-encounter getaway.
//   0xC0  Down   RCMessage 0x12 -> UI_InGame_WorldMap.fng
//   0xC1  Left   RCMessage 0x13 -> UI_Status_Master.fng
//   0xC2  Right  SMSMessageQueue::SummonMessageViewer (0x8012C274); the
//                mailbox proper, UI_SMS_Mailbox.fng, is RCMessage 0x0D.
//
// Up lighting the tab and Right lighting the envelope is the cross-check: the
// .fng geometry puts the tab above the rosette and the envelope to its right,
// and the enum ordering predicts exactly those two.
//
// What this port draws, and what it does not
// ------------------------------------------
// The steady state. EventIndicator::SetResetState (0x80111988, and the PC
// build's own copy at SPEED2.EXE 0x004DAA4C -- the two are instruction for
// instruction the same) is what runs when there is no event to show: it points
// the rosette's centre at Engage_Cingular_Icon (0xEA570722) and hides the tab,
// both arrows, the envelope and the count line. EngageEvent::UpdateSMS sets the
// same centre texture (0x80111200, 0x801112ac) and adds the envelope with the
// right arrow. So with nothing to engage the centre is the Cingular icon.
//
// An event or a shop changes it. EngageEvent::UpdateGeneric puts
// ENGAGE_PHONE_ICON (0x6FFF0767) in the centre in every state the event is
// live in -- SPEED2.EXE 0x4C8BB2 on the first frame and 0x4C8C6A after -- and
// gives it the "Pulse" script, the arrow "pulse" and the tab "Blink". Finish
// (state 6) puts Engage_Cingular_Icon back with "Init". The event outranks
// the SMS one (GetEventPriority 0x80111768: 4 against 2) and
// EventIndicator::Update (0x801119E8) runs the live event last, so the phone
// stays up with mail waiting too. The scripts are decoded and played; engage.c
// has their keys.
//
// The GameCube build differs here, and the port follows the PC: after the
// first frame Speed.elf's UpdateGeneric swaps the phone for the console's own
// D-pad picture (DetermineEngageType 0x8011095C, 3RDPERSON_DPAD_GAMECUBE or
// 0xBC45EF36 by controller), where SPEED2.EXE keeps the phone. The user's PC
// screenshot shows the phone.
//
// Colours are the widgets' own SA colour tracks, read B,G,R,A the way fng.h
// documents: the rosette centre and the envelope both hold 0x30,0x80,0xCF,0xFF,
// which is red 207, green 128, blue 48 -- Cingular's orange, not a blue. The
// panel and the rosette are white at alpha 100 and 200, and they multiply art
// that is already coloured: Engage_Backing is black, and Engage_DPAD_Backing is
// a dark ring with four grey (154,154,154) arrows baked into it, which is why
// all four directions are visible without any of them being lit.
//
// The tab, guid 900478, is drawn for a shop. UpdateGeneric puts the event's
// own icon in it, and GetEventTypeTexHash (0x801115F0) is the table that picks
// the art from an EventIndicatorType. For a SHOP_TRIGGER the type is not a
// career record's: _get_shop_indicator_type (0x80111CA4) reads the kind byte
// at ShopDataDesc+0x50 and maps it straight to one, so the whole chain from
// the polygon the car is standing in to the texture on the tab is shipped data
// end to end -- zone handle -> FindShopDataDescByTriggerZone (0x80121F54) ->
// kind -> EventIndicatorType -> texture hash. See kShopIndicatorType and
// kEventTexHash in engage.c for both tables, and eventdb.h for the validation
// that pins the two record fields down.
//
// A RACE_START_TRIGGER still lights the arrow without a tab: its handle
// resolves to a CareerEventData, and that table's event-type field is E4. The
// arrow says "D-pad Up does something here", which is known either way.
//
// Not drawn, because the port has nothing to drive them with yet (none of this
// is a guess left in):
//   * the arrival animation, UpdateSMS state 3. It swaps the envelope for the
//     message's category art and tints it from
//     SMSGetEnvelopeImageHashAndColour (0x800DC308):
//       1 Engage_SMS_Rachel_Message        0x88C9990A
//       2 Engage_SMS_Opponent_Message      0x8B2F36EE
//       3 Engage_SMS_Special_Event_Message 0x470222FD
//       4 Engage_SMS_Outrun_Message        0xA9DFE648
//       5 Engage_SMS_Shop_Message          0x5D9B6555
//       6 Engage_SMS_Tips_Message          0x756B5C1B
//       7 Engage_SMS_Race_Message          0x55476A96
//       - Engage_SMS_mail                  0x4B9B8F5A
//     The category is not in the save: UpdateSMS reads it from the message's
//     SMSMessageData record (+0x05), and the save slot holds only that record's
//     name hash. Decoding the SMSMessageData table is its own task.
//   * the FEng scripts UpdateSMS attaches -- "static" on the right arrow and
//     the envelope, "Hide" on the count line. UpdateGeneric's three (PULSE,
//     BLINK) are played; the SMS arrow's STATIC colour has a channel past
//     255 and no keys, and how FEng narrows that is not known, so that arrow
//     stays white at full alpha.
//   * the count line, guid 923595. UpdateSMS state 4 does make it visible, but
//     it then prints the empty string into it (FEngPrintf with the format at
//     Speed.elf 0x80322504, which is four nul bytes), so retail shows nothing
//     there. The authored "x3" is the designer's placeholder.
//
// Nothing is substituted. 3rdperson_DPAD_PS2 is only what the .fng authors into
// the centre slot at design time; no code path ever shows it, because both
// Updates overwrite that slot before the first draw.
//
// Scale and anchor
// ----------------
// The same half scale the rest of the HUD uses (hud.h): the 640x480 canvas
// against a 400x240 screen. hud.c anchors the canvas's right and bottom edges
// to the screen's because its cluster lives in that corner; this group anchors
// the left and top edges, for the same reason.

typedef struct Engage Engage;

// `freeRoamBunPath` is GLOBAL/InGameFreeRoam.bun, whose
// Global\HudTexturesSingleRace.tpk holds the panel, rosette, arrows and
// envelope. `globalBunPath` is GLOBAL/GLOBALB.BUN, which is where the tab's
// own art lives: none of the MODE_ICON_* / SHOP_ICON_* textures is in
// InGameFreeRoam.bun, so the tab needs the second pack. Either may be NULL,
// and a missing file or texture costs only the pieces it holds -- whatever
// loaded still draws. Returns NULL only if the allocation fails.
Engage* engage_open(const char* freeRoamBunPath, const char* globalBunPath,
	void (*log)(const char*));

void engage_close(Engage* e);

// _get_shop_indicator_type (Speed.elf 0x80111CA4): the EventIndicatorType for
// a shop's kind byte (ShopRec::kind, eventdb.h). Pass the result to
// engage_draw() as `indicatorType`. Retail's default for an unlisted kind is
// 1, which is the circuit icon; the only shipped kind that takes it is 6,
// THE_UNIQUES_SHOP, which has no free-roam trigger to reach it by.
int engage_shop_indicator_type(int shopKind);

// 0 is retail's "nothing to show": GetEventTypeTexHash returns a zero hash for
// it and EventIndicatorType is 1-based.
#define ENGAGE_INDICATOR_NONE 0

// Draws onto the current citro2d target, which must be the top screen.
// `smsWaiting` is Profile::smsUnread != 0: it adds the envelope and lights the
// right arrow, exactly as UpdateSMS state 4 does. `canEngage` is "the car is
// standing in a trigger polygon" -- it lights the up arrow and puts the pulsing
// phone in the centre, the half of UpdateGeneric that does not need the event
// record (see above). Its scripts are timed from the first frame it is true.
// `indicatorType` is an EventIndicatorType, or ENGAGE_INDICATOR_NONE: it draws
// the tab above the arrow with that type's own art. A type whose texture is
// not in either pack draws no tab rather than a substitute.
void engage_draw(Engage* e, bool smsWaiting, bool canEngage, int indicatorType);

// The prologue's phone event (ENGAGE_EVENT_STAGE0): its slot's state from
// engage_events_state(), set before engage_draw(). 2 pulses the phone in the
// centre, 3 and 4 hold it (EngageEvent::UpdateCareerStage0 0x80110D40); with
// no zone and no mail it takes the Cingular logo's place.
void engage_set_stage0_phone(Engage* e, int slotState);

// PORT (#447): the D-pad direction each lit arrow stands for -- Enter Shop's
// (retail's up) and the SMS's (retail's right) -- from the profile's binding:
// 0 up, 1 right, 2 down, 3 left, or ENGAGE_DIR_BUTTON when the action is on a
// button, which lights no arrow; the caller draws that button's badge at
// engage_key_anchor() instead. Retail's up and right until set.
#define ENGAGE_DIR_BUTTON (-1)
void engage_set_dirs(int shopDir, int smsDir);

// The screen point of the arrow for direction `dir` (0..3), for a badge
// drawn in its place.
void engage_key_anchor(int dir, float* x, float* y);

// ---------------------------------------------------------------- the events
//
// What the widget announces, and the sound it makes doing it: EventIndicator's
// eight EngageEvents (EventIndicator +0x60, 0x14 bytes each) and their
// state machine, apart from the art above. A slot is 0 empty, 1 waiting
// (Fill), 2 and 3 its arrival (Start, then updateState), 4 shown.
//
//   FEngHud::EnterTriggerZone (0x801136D4)  -> AddEvent(the zone's type, 0)
//   FEngHud::ExitTriggerZone (0x80113804)   -> RemoveEvent(that type)
//   SMSMessageManager::Update (0x80113978)  -> AddEvent(0x13, unread) when the
//       unread count rises past the last one it saw, RemoveEvent(0x13) at none
//   EventIndicator::Update (0x801119E8), once a frame: every slot's
//       EngageEvent::Update (0x80111538); then, with no slot in 2 or 3, the
//       highest-priority one (GetEventPriority 0x80111768) is Started unless
//       it is already 4; with one arriving, a higher one Overides it
//       (0x80110A6C: an SMS drops back to 4, anything else Finishes) and
//       Starts.
//
// EngageEvent::playSound (0x80110C88) is every sound the widget makes, each
// MakeFrontendSound(event 0xB, trigger) through FrontendSoundMap:
//   Start (0x801109C0)            trigger 0xF for an SMS, else 0xD
//   updateState (0x80110AF0) 2->3 trigger 0xE, once the world timer is past
//                                 1 s from Start (for an SMS, not while the
//                                 prologue has control)
//                            3->4 at 4 s, silent (playSound(4) has no arm)
// Type 0x14, the prologue's phone (TriggerStage0Intro), is silent throughout
// and holds 2 for its count in seconds.
// Player::EnterEventTriggerZone's types for the two zones that are neither
// a race nor a shop (#268 K): ICON_SHOWCASE for a showcase's trigger and
// MODE_ICON_WORLD_CHALLENGE for a world challenge's start.
// FEngHud::NotifyRandomEncounterCar's (#268 H step 2): ICON_RANDOM_ENCOUNTER
// while an Outrun is on offer.
#define ENGAGE_EVENT_RANDOM_ENCOUNTER 0x09
#define ENGAGE_EVENT_SHOWCASE  0x11
#define ENGAGE_EVENT_CHALLENGE 0x12
#define ENGAGE_EVENT_SMS    0x13
#define ENGAGE_EVENT_STAGE0 0x14
#define ENGAGE_EVENT_SLOTS  8

#define ENGAGE_SND_EVENT       0xB  // the FrontendSoundEvent row
#define ENGAGE_SND_ARRIVE      0xD
#define ENGAGE_SND_ARRIVE_LATE 0xE
#define ENGAGE_SND_ARRIVE_SMS  0xF

typedef struct {
	signed char state;  // +0
	signed char type;   // +1, an EventIndicatorType
	float start;        // +4, the world timer at Start, in seconds here
	int count;          // +0x10, AddEvent's second argument
} EngageEvent;

typedef struct {
	EngageEvent ev[ENGAGE_EVENT_SLOTS];
	float now;          // the world timer (r13 - 0x4F00, 4000 ticks a second)
	int smsSeen;        // SMSMessageManager +0, 0 when the HUD is built
	// Plays FrontendSoundMap[ENGAGE_SND_EVENT][trigger]; may be NULL.
	void (*sound)(int trigger);
} EngageEvents;

// A new HUD: every slot empty and no mail seen, so mail already waiting is
// announced again on the first update.
void engage_events_reset(EngageEvents* q, void (*sound)(int trigger));
void engage_events_add(EngageEvents* q, int type, int count);
void engage_events_remove(EngageEvents* q, int type);
// SMSMessageManager::Update with `unread` as GetNumMessagesUnread(true).
void engage_events_sms(EngageEvents* q, int unread);
// The state of the first slot holding `type`, 0 with none.
int engage_events_state(const EngageEvents* q, int type);
// EventIndicator::Update, `dt` seconds of world time on. `stage0Control` is
// Stage0StateManager::HasControl.
void engage_events_update(EngageEvents* q, float dt, bool stage0Control);
