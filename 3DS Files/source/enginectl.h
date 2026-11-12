// The cars' engine sound controllers: retail's SFXCTL objects of the
// PlayerCar state, which turn the car's revs, throttle and gear changes into
// the revs, torque and mix the engine sound plays (CARSFX_PlayerEngine,
// carsfx.c) and into output words the mix map reads (mixmap.h); and the
// AICar state's, which make the same up for an opponent from how it moves
// (CARSFX_AIEngine). TASKS.md #298.
//
//   SFXCTL_Physics     0x60010000  ctor 0x80094580, InitCTL 0x80094A30,
//                                  UpdateParams 0x8009469C,
//                                  UpdateMixerOutputs 0x80094AB4
//   SFXCTL_Shifting    0x60010030  SetupCTL 0x8008FFBC, InitCTL 0x8009009C,
//                                  UpdateParams 0x80090700,
//                                  UpdateGearShiftState 0x80090110,
//                                  BeginUpShift 0x80090910,
//                                  BeginDownShift 0x80090A64,
//                                  UpdateMixerOutputs 0x8008FE78
//   SFXCTL_AccelTrans  0x60010040  SetupCTL 0x80092954, InitCTL 0x800929FC,
//                                  UpdateParams 0x80092AF4,
//                                  UpdateState 0x80092CC8
//   SFXCTL_Engine      0x60010050  ctor 0x80090E98, InitCTL 0x800910B8,
//                                  UpdateParams 0x800912D8
//   SFXCTL_HybridMotor 0x60010060  SetupCTL 0x800934CC,
//                                  UpdateParams 0x80093874,
//                                  UpdateMixerOutputs 0x80094444
//
// An opponent's, the AICar state's (type 2), one copy per racer:
//
//   SFXCTL_AIPhysics   0x60020000  ctor 0x80094EC4, InitCTL 0x80094FC8,
//                                  UpdateParams 0x80095014,
//                                  GenDeltaRPM 0x800952EC,
//                                  UpdateRPM 0x800954AC,
//                                  UpdateAccel 0x800955DC,
//                                  SuggestGear 0x8009570C,
//                                  UpdateGear 0x800958CC
//   SndAIStateManager  (AIPhysics +0xFC) Initialize 0x8008D410,
//                                  Update 0x8008D5B4,
//                                  UpdateState 0x8008D7AC
//   SndAITrigger       ctor 0x8008D0C8, Update 0x8008D1F8
//   SFXCTL_AIShifting  0x60020030  Shifting's, with no mixer outputs
//                                  (UpdateMixerOutputs 0x800BEB7C)
//   SFXCTL_AccelTrans  0x60020040  and SFXCTL_Engine 0x60020050, the
//                                  player's classes. The state has no
//                                  HybridMotor: no Ginsu, the AEMS graph alone.
//
// The traffic's engines (the Traffic state) are not ported here; their sound
// is carsfx.c's CARSFX_TrafficEngine (#316).
#pragma once

#include "engine_tables.h"

#include <stdbool.h>
#include <stdint.h>

// What CarPreSetup (EAXCar, SPEED2.EXE's twin of Speed.elf 0x8007FBB8) and
// the car's physics block hand the controllers once, when the world loads.
typedef struct {
	int   engIdx;                 // CSTATE +0x78: `eng`'s kEngineData entry
	const EngineData*    eng;     // CSTATE +0x7C
	const DualGinsuData* dual;    // CSTATE +0x80, used only when eng->dual (+0x74)
	const ShiftPattern*  shift;   // Shifting +0x7C
	const AccelFromIdle* accel;   // AccelTrans +0xD8: kAccelFromIdle[eng->accelIdx]
	int   engLevel;               // CSTATE +0x90: 0..3 by the engine parts fitted
	int   transLevel;             // CSTATE +0xA0: the transmission upgrade, 0..3
	float idle, redline, limiter; // CarPhysicsInfo EngineInfo +0x1F0, +0x1F4, +0x1F8
	uint32_t* rng;                // bRandom's state, the one every draw in the game shares
	// An opponent's only, the race's kind (AIPhysics reads it every frame):
	bool  drift;                  // DAT_803FCA84 || g_tweakIsDriftRace
	bool  shortTrack;             // DAT_803FCA98 || g_tweakIsShortTrackRace: Street X
} EngineCtlSetup;

// Five opponents at most: the AICar state's copies (mix map).
#define ENGINECTL_AI_MAX 5

// An opponent, each frame: what AIPhysics and SndAIStateManager read off its
// Car and CarState (the AI car has no engine the sound can read).
typedef struct {
	float    speed;     // Car +0x440: |velocity|, m/s
	float    dSpeed;    // Car +0x440 less |Car +0x518|: the speed gained over a 60 Hz step
	uint16_t steer;     // CarState +0x3DE: the wheels' angle, a turn in 65536
	bool     nitro;     // CarState::IsNitrousOn
	float    throttle;  // CarDriver +0x1F0, 0..1
	int      gear;      // Car +0x444, retail's numbering: 0 reverse, 1 neutral, 2 first
	int      numGears;  // CarPhysicsInfo: the forward gears
	float    topSpeed;  // Car::GetTopSpeed, m/s
	float    upRpm;     // Car::GetOptimalShiftUpRPM(gear)
	bool     nis;       // SFXCTL_AIPhysics +0xD8: a shop / end NIS, or an intro NIS before racing
} EngineCtlAiIn;

// The car, each frame.
typedef struct {
	int   gear;        // Car +0x444, retail's numbering: 0 reverse, 1 neutral, 2 first
	float rpm;         // Car +0x450: the engine's revs
	float throttle;    // the pedal, 0..1
	float speed;       // Car +0x440: m/s (its size is what is read)
	int   wheelsDown;  // Car::GetWheelsOnGround
	bool  shifting;    // Car::IsShifting
	float upRpm;       // Car::GetOptimalShiftUpRPM(gear)
	int   povType;     // CSTATE +0xA4: CARDB_POV_* of the view in use
	bool  human;       // DriverInfo +4 == 1: the player, not a ghost or a demo driver
	int   zone;        // Car +0x18's Player +0x58: the player's zone (main.c, 6 the crash camera)
	bool  cubicCam;    // Player +0x224's +8 == 1: the camera mover is a CubicCameraMover
	bool  nis;         // SFXCTL_Physics +0xD8: a shop, end or intro NIS is playing (#520)
} EngineCtlIn;

// What CARSFX_PlayerEngine reads off the controllers.
typedef struct {
	bool    dual;      // CSTATE +0x74: a decel Ginsu beside the accel one
	float   ginRpm;    // HybridMotor +0xFC: the Ginsu RPM
	int32_t aemsVol;   // HybridMotor +0x7C: the AEMS CAR graph's level
	int32_t ginVol;    // HybridMotor +0x80: the accel Ginsu's level
	int32_t decelVol;  // HybridMotor +0x84: the decel Ginsu's level
	int32_t redVol;    // HybridMotor +0xB8: the AEMS redline sample's level
	float   lowpass;   // HybridMotor +0xBC: the Ginsu low-pass cutoff, Hz
	float   torque;    // SFXCTL_Engine::GetEngTorque (+0x12C), 0..100
	int32_t f130;      // SFXCTL_Engine +0x130, 150 from InitCTL
	float   engRpm;    // SFXCTL_Engine::GetEngRPM (+0x124), 1000..10000 and past
	int32_t engVol;    // SFXCTL_Engine +0xDC: its level, 0x7FFF and the shift's dip
	// What CARSFX_Shift, CARSFX_Turbo and CARSFX_SparkChatter read: CSTATE,
	// and Shifting's one-frame flags (each 1 for the one frame it happens).
	int     engIdx;    // CSTATE +0x78: the kEngineData entry, CARDATAMAPPING[row][level][0]
	float   physTrq;   // CSTATE +0x44: Physics' throttle, 0..100
	float   sndRpm;    // CSTATE +0x48: the revs on the sound's scale, 1000..10000
	int     gear;      // CSTATE +0x50: 0 reverse, 1 neutral, 2 first
	float   thrPct;    // CSTATE +0x54: the throttle, 0..100
	int     holdOver;  // Shifting +0x70: the post-shift hold just ran out
	int     upBegan;   // Shifting +0x74: an up-shift just began
	int     upCaught;  // Shifting +0x78: an up-shift just reached its state 2
	int     shiftState;// Shifting +0x80: 0 none, 1..3 an up-shift, 4..7 a down-shift
	float   rpmFrom;   // Shifting +0xB0: the revs the frame before the shift
} EngineCtlMix;

// SetupCTL + InitCTL of every controller. NULL (or no `s->eng`) turns them
// off, and enginectlMix() then reports silence.
void enginectlSetup(const EngineCtlSetup* s);
// One frame: every controller's UpdateParams, then their UpdateMixerOutputs,
// in ID order, as the PlayerCar state runs them -- before the mix map.
void enginectlUpdate(const EngineCtlIn* in, float dt);
const EngineCtlMix* enginectlMix(void);
// The player car's EAXCar::SFXMessage (0x8007FED4) from a NIS's EvtTrk
// (CAnimEvent::PlayEvent 0x8004DC6C, types 0xD and 0xE): 7, CAR_AUDIO_PATTERN
// <car> <id> <scale>, and 8, CAR_AUDIO_REV <car> <ticks> <amount>. UpdateNIS
// plays them while EngineCtlIn.nis is set (#520).
void enginectlNisPattern(int id);
void enginectlNisRev(float ticks, float amount);
// The same for an opponent's copy (EAXAITunerCar, #497).
void enginectlAiNisPattern(int copy, int id);
void enginectlAiNisRev(int copy, float ticks, float amount);

// The same for opponent `copy` (0 .. ENGINECTL_AI_MAX - 1): the AICar state's
// controllers, AIPhysics, AIShifting, AccelTrans and Engine. NULL turns that
// copy off; enginectlAiShutdown turns off all of them.
void enginectlAiSetup(int copy, const EngineCtlSetup* s);
void enginectlAiUpdate(int copy, const EngineCtlAiIn* in, float dt);
const EngineCtlMix* enginectlAiMix(int copy);
void enginectlAiShutdown(void);
