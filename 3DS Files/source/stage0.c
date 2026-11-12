// Stage0StateManager -- see stage0.h for the retail layout and the machine.

#include "stage0.h"

#include <stdio.h>
#include <string.h>

#include "dbglog.h"

static void dlog(const char* msg) { dbglog_line(NULL, msg); }

// The one wait every timed state uses: 2.0f seconds, compared in retail
// against (WorldTimer - stamp) * 0.00025f. Kept as seconds here because the
// port's world tick is already in seconds; the 1/4000 tick count would be the
// same number either way.
#define STAGE0_WAIT 2.0f

typedef struct {
	int   state;       // +0x00
	float since;       // +0x04, as seconds elapsed rather than a stamp
	bool  gate[2];     // +0x08, +0x0C
	bool  gpsCarLot;   // MaybeTargetGPSForShopEvent has run
	int   gpsCalls;    // ... and how many times since the caller last asked
	bool  smsRaised;   // AddExternalMessage("SMS_INSTRUCTION") has run
	bool  smsPopup;    // ... and DoAutopopup after it (3 -> 5 only)
	bool  phone;       // TriggerStage0Intro is due (state 1), not yet taken
} Stage0;

static Stage0 s;

static void enter(int next)
{
	s.state = next;
	s.since = 0.0f;
	char msg[96];
	snprintf(msg, sizeof(msg), "stage0: -> state %d", next);
	dlog(msg);
}

// Retail's per-state speech calls. Each one is
// SpeechCmdCareer(CR_Event, CharId 0x20) into DoSpeechCall; here it is the
// line the host-side decode resolved for exactly those parameters.
//
// PORT: state 1 does not speak in retail -- it raises the phone
// (FEngHud::TriggerStage0Intro -> EventIndicator::AddEvent(type 0x14, 2)) and
// the phone widget starts Stage0Intro when the call is answered. The port has
// no phone screen yet (#274 c), so the intro is started where the phone would
// have started it. When the screen lands, move this call behind it.
static void speak(SpeechLine line)
{
	speechPlay(line);
}

// CareerSpeech::Stage0IsSpeechDone (0x8018A998). A card with no speech set
// never reports a line playing, so the machine walks through on its timers
// instead of stalling -- which is why every state tests the wait as well.
static bool speechDone(void)
{
	return !speechIsPlaying();
}

static bool waited(void)
{
	return s.since > STAGE0_WAIT;
}

void stage0Reset(void)
{
	memset(&s, 0, sizeof(s));
}

bool stage0NotifyBeginIntro(bool nisBusy)
{
	// 0x80130388: state 0 -> 1, but never over an NIS.
	if (s.state != 0 || nisBusy)
		return false;
	dlog("stage0: the prologue begins -- the car is not the player's yet");
	enter(1);
	return true;
}

void stage0Update(float dt)
{
	if (s.state <= 0 || s.state >= 9)
		return;
	s.since += dt;

	switch (s.state) {
	case 1:
		// FEngHud::TriggerStage0Intro(hud, 2): the phone indicator goes up
		// (stage0TakeIntroTrigger()). See speak()'s note -- the port starts
		// the line here as well.
		s.phone = true;
		speak(SPEECH_STAGE0_INTRO);
		enter(2);
		break;

	case 2:
		if (waited())
			enter(3);
		break;

	case 3:
		// The one state that does not wait: retail tests only the speech and
		// then jumps past 4.
		if (speechDone()) {
			enter(5);
			speak(SPEECH_STAGE0_SMS);
			// SMSMessageQueue::AddExternalMessage(bStringHash("SMS_INSTRUCTION")),
			// FEngHud::NotifySMSMessage(true, 1), DoAutopopup().
			s.smsRaised = true;
			s.smsPopup = true;
			dlog("stage0: SMS_INSTRUCTION raised");
		}
		break;

	case 4:
		// Nothing enters 4 in the shipped flow; ported as retail has it.
		if (waited() && speechDone())
			enter(5);
		break;

	case 5:
		if (waited() && speechDone() && s.gate[0] && s.gate[1]) {
			enter(6);
			speak(SPEECH_STAGE0_MINIMAP);
		}
		break;

	case 6:
		if (waited() && speechDone()) {
			enter(7);
			// PlayerCareerState::RecalcUnlockedZones() and
			// MaybeTargetGPSForShopEvent(bStringHash("DDAY_EVENT_A")). The
			// recalc is implicit in the port: the world's trigger search is
			// suppressed only while stage0HasControl(), and the car lot is
			// a shop zone, which is why it is the one thing reachable.
			s.gpsCarLot = true;
			s.gpsCalls++;
			dlog("stage0: GPS -> DDAY_EVENT_A (the car lot)");
			speak(SPEECH_STAGE0_CARLOT);
		}
		break;

	case 7:
		// Retail calls Stage0IsSpeechDone here and throws the answer away --
		// the result is never tested, unlike every other state. Kept as the
		// discarded call it is.
		(void)speechDone();
		if (waited())
			enter(8);
		break;

	case 8:
		stage0RelinquishControl();
		break;

	default:
		break;
	}
}

bool stage0TakeIntroTrigger(void)
{
	const bool t = s.phone;
	s.phone = false;
	return t;
}

bool stage0HasControl(void)
{
	return s.state != 0 && s.state != 9;
}

void stage0RelinquishControl(void)
{
	// 0x801303F4: state 9, SetInputMode(player, 2), RecalcUnlockedZones(),
	// AddExternalMessage(hash("SMS_INSTRUCTION")), NotifySMSMessage(true, 1),
	// MaybeTargetGPSForShopEvent(hash("DDAY_EVENT_A")). A skipped prologue
	// lands in exactly the state a played-out one does, which is the whole
	// point of repeating the SMS and the GPS target here.
	s.state = 9;
	s.since = 0.0f;
	s.smsRaised = true;
	s.gpsCarLot = true;
	s.gpsCalls++;
	dlog("stage0: control handed back -- the car is the player's, "
	     "the car lot is on the GPS");
}

void stage0RequestSkip(bool allowed)
{
	if (allowed && stage0HasControl())
		stage0RelinquishControl();
}

void stage0SetTutorialCompleted(void)
{
	s.state = 9;
	s.since = 0.0f;
}

// NotifyReadMessage (0x80130668) and NotifySMSDialogDismissed (0x80130674),
// the two setters state 5 waits on. Both are a bare `store 1` in retail --
// NotifyReadMessage even takes a hash it never looks at, because
// SMSMessageQueue::NotifyReadMessage (0x8012C588) passes one.
void stage0NotifyReadMessage(void)
{
	s.gate[0] = true;
}

void stage0NotifySmsDialogDismissed(void)
{
	s.gate[1] = true;
}

// ShowTelephone (0x801304D4), ShowMinimap (0x80130580), ShowGauges
// (0x801305CC) and ShowCarLot (0x80130618). Each is the same shape: false
// unless HasControl, then one test on the state. FEngHud::DetermineHudFeatures
// (0x802158F8) reads them only while the prologue has control, where its
// whole feature mask is these bits: ShowTelephone or ShowEngageEventThingy
// the EventIndicator, ShowMinimap the minimap, ShowGauges the tach and turbo.
// Everything else -- the bank, the nitrous, the speedo's neighbours -- is off
// until HasControl goes false and the normal mask takes over (#274).
bool stage0ShowTelephone(void)
{
	return stage0HasControl() && s.state == 2;
}

// ShowEngageEventThingy (0x8013051C): past the phone, except while state 5
// still waits with the instruction SMS already read -- the widget steps aside
// for the reader.
bool stage0ShowEngageEventThingy(void)
{
	return stage0HasControl() && s.state > 2 && !(s.state == 5 && s.gate[0]);
}

// The compares are `cmpwi state, 5` / `8` read back through the CR's GT bit
// (rlwinm 30,31,31): greater than, not equal. So the minimap comes back at
// state 6 and stays, and ShowGauges is never true under HasControl (9 is
// not control) -- the gauges stay off for the whole prologue.
bool stage0ShowMinimap(void)
{
	return stage0HasControl() && s.state > 5;
}

bool stage0ShowGauges(void)
{
	return stage0HasControl() && s.state > 8;
}

bool stage0ShowCarLot(void)
{
	return stage0HasControl() && s.state >= 7;
}

int stage0State(void)
{
	return s.state;
}

bool stage0GpsTargetsCarLot(void)
{
	return s.gpsCarLot;
}

bool stage0TakeGpsCall(void)
{
	if (s.gpsCalls <= 0)
		return false;
	s.gpsCalls--;
	return true;
}

bool stage0SmsRaised(void)
{
	return s.smsRaised;
}

bool stage0SmsAutopopup(void)
{
	return s.smsPopup;
}

void stage0ClearSms(void)
{
	s.smsRaised = false;
	s.smsPopup = false;
}
