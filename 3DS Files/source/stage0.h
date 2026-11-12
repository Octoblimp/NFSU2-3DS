// Stage0StateManager -- the career prologue.
//
// Everything the player sees on a new career's first drop into the world is
// this one object: Rachel's call, the car held at the airport until she is
// done, the GPS then pointed at the car lot, and a world with nothing else in
// it. Retail keeps it at PlayerCareerState + 0x6824 (see
// PlayerCareerState::NotifyBeginStage0Intro, Speed.elf 0x80126CAC) and drives
// it from Update (0x80130680).
//
// Layout, retail's:
//     +0x00  the state: 0 not begun, 1..8 running, 9 done
//     +0x04  the WorldTimer stamp the current state started at
//     +0x08  two flags state 5 waits on, set from outside Update
//     +0x0C
//
// Every timed step is the same wait: `(WorldTimer - stamp) * 0.00025f > 2.0f`
// in state 2, and `Timer::SetTime(2.0f)` compared against the same difference
// in 4..7. 0.00025 is 1/4000, so WorldTimer counts the same 1/4000 s ticks the
// best-lap times are in (profile_save.h). Two seconds, every time.
//
// The machine:
//     1  stamp, FEngHud::TriggerStage0Intro(hud, 2)                    -> 2
//     2  wait 2 s                                                      -> 3
//     3  speech done -> 5, stamp, Stage0SMS(),
//        SMSMessageQueue::AddExternalMessage(hash("SMS_INSTRUCTION")),
//        FEngHud::NotifySMSMessage(true, 1), DoAutopopup()
//     4  wait 2 s and speech done                                      -> 5
//     5  wait 2 s, speech done, and both flags set -> 6, stamp,
//        Stage0Minimap()
//     6  wait 2 s and speech done -> 7, stamp,
//        PlayerCareerState::RecalcUnlockedZones(),
//        MaybeTargetGPSForShopEvent(hash("DDAY_EVENT_A")), Stage0Carlot()
//     7  wait 2 s                                                      -> 8
//     8  RelinquishControl()
//
// Nothing enters state 4 in the shipped flow -- state 3 jumps straight to 5 --
// and it is ported anyway because RequestSkip and a reload can land there.
//
// THE AIRPORT LOCK is the input mode, not a flag of its own:
// RelinquishControl (0x801303F4) is the only caller of
// Player::SetInputMode(player, 2), so for the whole of states 1..8 the car
// simply is not the player's. HasControl (0x801304B0) is
// `state != 0 && state != 9` and means "the tutorial is driving".
//
// WHY NO RACES SHOW falls out of the order rather than being a rule:
// RecalcUnlockedZones is called exactly twice, at 6 -> 7 and inside
// RelinquishControl. Until then no trigger zone resolves, so there is no event
// circle, nothing on the minimap, nothing on the GPS and nothing startable --
// and the car lot is the only place to go. Stage 0's races then wait on
// CareerSpeech too: BuildListOfEvents offers them only once
// NotifyCSCanSeeEvents has run, 15 s of career time into DDAY_EVENT_A, and no
// more after NotifyCSStage0Timeout or on DDAY_EVENT_B (main.c's event pile,
// TASKS.md #339).
#pragma once

#include <stdbool.h>

#include "speech.h"

// Default (0x80130374): state 0, no stamp, both flags clear.
void stage0Reset(void);

// NotifyBeginIntro (0x80130388): 0 -> 1. Retail refuses while
// CAnimManager::IsPlayingShopNIS or IsLoadingShopNIS -- the prologue never
// starts over an NIS -- so `nisBusy` carries that test. Returns true when the
// prologue actually started.
bool stage0NotifyBeginIntro(bool nisBusy);

// Update (0x80130680). `dt` in seconds.
void stage0Update(float dt);

// State 1's FEngHud::TriggerStage0Intro(hud, 2) (0x80114E1C), the HUD's
// EventIndicator::AddEvent(0x14, 2): true once, the first call after state 1
// has run. State 1 lasts a single update, so polling stage0State() for it
// misses it.
bool stage0TakeIntroTrigger(void);

// HasControl (0x801304B0): the tutorial is driving, so the player is not.
// This is what holds the car at the airport and what suppresses the world's
// event triggers.
bool stage0HasControl(void);

// RelinquishControl (0x801303F4): state 9, the input mode handed back,
// RecalcUnlockedZones, the SMS raised again and the car lot targeted. A
// skipped prologue lands in exactly the state a played-out one does.
void stage0RelinquishControl(void);

// RequestSkip (0x80130484). Retail gates it on a global flag whose setter is
// not worked out yet, so the port takes `allowed` from the caller.
void stage0RequestSkip(bool allowed);

// SetTutorialCompleted (0x801303E8): state = 9, with none of
// RelinquishControl's side effects. What a career loaded past the prologue
// gets.
void stage0SetTutorialCompleted(void);

// The two flags state 5 waits on (+0x08, +0x0C), each with the call that
// sets it. The prologue holds at state 5 until the player has opened the
// SMS_INSTRUCTION message and closed the reader again.
void stage0NotifyReadMessage(void);
void stage0NotifySmsDialogDismissed(void);

// The tutorial's four "look here" queries. All false outside the prologue,
// since each one starts on HasControl().
bool stage0ShowTelephone(void);          // state 2
bool stage0ShowEngageEventThingy(void);  // past 2, but not 5 once the SMS is read
bool stage0ShowMinimap(void);            // state 6 and after
bool stage0ShowGauges(void);             // past 8: never, under control
bool stage0ShowCarLot(void);             // state 7 and after

int stage0State(void);

// True once state 6 -> 7 has run MaybeTargetGPSForShopEvent. The target is
// always DDAY_EVENT_A, the career's first shop event -- the car lot.
bool stage0GpsTargetsCarLot(void);

// One MaybeTargetGPSForShopEvent(hash("DDAY_EVENT_A")) call the machine has
// made -- at 6 -> 7 and again in RelinquishControl -- for the caller to carry
// out against the career's tables; true once per call, then false.
bool stage0TakeGpsCall(void);

// The SMS state 3 raises (and RelinquishControl again): true from the moment
// AddExternalMessage runs until the world queues it and clears it (#274 c).
bool stage0SmsRaised(void);
// Whether DoAutopopup followed it, as it does at 3 -> 5 and not in
// RelinquishControl: the world then opens the reader on the message itself.
bool stage0SmsAutopopup(void);
void stage0ClearSms(void);
