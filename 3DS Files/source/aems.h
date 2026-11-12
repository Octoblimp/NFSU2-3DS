// Retail EA AEMS sound modules, run on the 3DS (TASKS.md #298).
//
// Every car and world sound in NFSU2 that is not music or speech -- engines,
// nitrous, skids, shifts, turbo, wind, road noise, the frontend clicks -- is
// an AEMS "module": a sound graph EA compiled into a module bank (PC
// SOUND/*/*.abk). The game never plays a sample itself. It creates an instance
// of a Csis class ("FX_NITROUS", "CAR", ...), feeds it member data every frame
// (level, volume, azimuth, rpm, ...), and the bank's graph decides what plays,
// at what pitch and volume, when to fade, and when it is done.
//
// tools/abk3ds.py converts a PC bank into a `.aem`: the module image byte for
// byte, its relocations and Csis table, each graph's x86 update code lifted
// into a small bytecode, and the samples as DSPADPCM. This file is the runtime
// that the retail engine (SNDAEMSI_*, Csis::*, Speed.elf 0x802D7840-0x802DA3AC
// and SPEED2.EXE 0x71FEEF-0x72101F) was:
//   * the 40 graph nodes, ported from the PC exe's own node table (0x817FE0),
//     so the state layouts are exactly the ones the PC banks were built for;
//   * the bytecode VM that walks a module's graph every 20 ms tick;
//   * a minimal Csis: class handles by name, instances, member data,
//     destructor clients and the reference count the game watches;
//   * the bank "player" node, which starts, steers and stops DSP voices
//     through aemsVoice*() below (audio.c owns the channels).
//
// Everything the graphs store as a pointer is a 32-bit value. On the 3DS that
// is the address itself; built for a PC host test (AEMS_HOST) it is an offset
// into one arena, so the same file runs both places.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct AemsBank AemsBank;
typedef void* AemsInst;     // a Csis class instance (Csis::Class*)
typedef void* AemsClass;    // a Csis class handle, looked up by name

// Loads a converted bank and subscribes its modules to their classes. `bus` is
// the mixer bus its voices play on (an AudioBus). NULL if the file is missing
// or not a version-1 .aem.
AemsBank* aemsLoad(const char* path, int bus);
// Destroys every live instance of the bank's modules (their voices stop),
// unsubscribes them and frees the bank.
void aemsUnload(AemsBank* bank);

// Csis::ClassHandle::Set. Returns the same handle for the same name every
// time; a name no loaded bank answers to still gets a handle.
AemsClass aemsClass(const char* name);
// Csis::Class::CreateInstance: every module subscribed to the class makes an
// instance of its graph, then all of them get `params` (the member data, one
// int per field, as many as the class has). NULL only when out of memory.
AemsInst aemsCreate(AemsClass cls, const int32_t* params);
// Csis::Class::SetMemberData.
void aemsSetMemberData(AemsInst inst, const int32_t* params);
// Csis::Class::Release: the game lets go. The graphs see their destructor
// trigger and wind down on their own.
void aemsRelease(AemsInst inst);
// Csis::Class::GetRefCount. 1 = only the game holds it, every module instance
// is gone; retail's CARSFX code releases the instance when it reads < 2.
int aemsRefCount(AemsInst inst);

// Csis::Class::SubscribeConstructor and SubscribeMemberData from the game's
// side, for a class a graph makes (a ControlClass node's, such as
// CAR_SputOutput): `fn` hears of each instance as it is made (`made` true)
// and of every member data sent to one after. `params` is the class's
// fields; the instance is for comparing only, it may be freed after.
typedef void (*AemsWatchFn)(AemsInst inst, const int32_t* params, bool made, void* user);
void aemsWatch(AemsClass cls, AemsWatchFn fn, void* user);
void aemsUnwatch(AemsClass cls, AemsWatchFn fn);

// Runs the 20 ms AEMS ticks that `seconds` of game time covers. Call once a
// frame from the thread that calls the functions above.
void aemsUpdate(float seconds);

// Live module instances across every bank (debug).
int aemsLiveInstances(void);

// The voice handle of every playing player node of every live graph, up to
// `max` (#351); how many.
int aemsPlayerHandles(int32_t* out, int max);

// ---------------------------------------------------------------- voices
//
// The DSP side of the bank player node. Implemented by audio.c on the 3DS (and
// by tools/aems_hosttest.c in a PC host test). A voice handle is >= 0; any
// call on a handle whose voice has ended or been stolen does nothing.
//
// A voice's level is gain * pan, per side, the way PC SND keeps the volume
// and the azimuth's speaker gains apart (the bus and master gains go on top in
// audio.c). `pitch` is the playback rate over the clip's own (1.0 = as
// recorded); like an SND voice, a voice remembers the clip it plays.
typedef struct {
	const void*     data;      // DSPADPCM frames (linear memory on the 3DS)
	uint32_t        bytes;
	uint32_t        samples;
	uint32_t        rate;      // Hz
	bool            loop;      // loops the whole clip until stopped
	const uint16_t* coefs;     // 16 DSPADPCM coefficients
	uint16_t        ps;        // first frame's predictor/scale
	int16_t         hist0, hist1;
} AemsClip;

int      aemsVoicePlay(const AemsClip* clip, int bus, float pitch, float gain,
                       float panLeft, float panRight);
bool     aemsVoiceAlive(int voice);
const AemsClip* aemsVoiceClip(int voice);
void     aemsVoiceSetPitch(int voice, float pitch);
void     aemsVoiceSetGain(int voice, float gain);
void     aemsVoiceSetPan(int voice, float panLeft, float panRight);
void     aemsVoicePause(int voice, bool paused);
// Fades the voice out (a few ms, #310); its channel is free once the fade ends.
void     aemsVoiceStop(int voice);
// Ends every fade at once, before the samples they read are freed.
void     aemsVoiceCutFades(void);
// Ends every voice still on one of these clips, now (#330); how many.
int      aemsVoiceDropClips(const AemsClip* clips, uint32_t count);
uint32_t aemsVoiceSamplePos(int voice);
// Voices sounding right now (debug).
int      aemsVoicesPlaying(void);
// #351: ends what nothing can stop any more -- an AEMS voice whose handle is
// not in `held` (aemsPlayerHandles), and an effects channel sounding with no
// voice on it and no fade running -- once it has been so for half a second,
// and logs each. How many it ended.
int      aemsVoiceReap(const int32_t* held, int n);

// PC SND's pan law: an EA azimuth (0 ahead, clockwise, 0x10000 a turn) to
// the left and right speaker gains. For voices SND plays outside a bank, such
// as the Ginsu engine synth's.
void     aemsAzimPan(int32_t azim, float* l, float* r);
