// NFSU2-3DS hardware audio mixer.
//
// Everything audible goes through the 3DS DSP via libctru's ndsp service.
// Music is stored as Nintendo DSPADPCM (.dspm, produced by tools/dspenc.c); a
// dedicated low-duty streaming thread reads it off the SD card and expands it
// to PCM16 (#310, audio.c musicDecode()), so the render thread never stalls on
// I/O. The DSP resamples and mixes. The effects are DSPADPCM the DSP expands.
//
// Bus layout is the real game's, reverse-engineered from the mixer volume
// setting names in PC Files/SPEED2.EXE (~0x393FC8-0x394028): AOMasterVol,
// AOFEMusicVol, AOIGMusicVol, AOSFXMasterVol, AOEngineVol, AOTireVol,
// AOSpeechVol. A voice's final gain is voiceGain * busGain * masterGain.
//
// NDSP channel budget (24 total):
//   0        cutscene / boot-movie stereo PCM16 (the pre-existing video path)
//   1        music stream A  (stereo PCM16; 2 is spare)
//   3        music stream B  (for crossfades and FE->IG handoff; 4 is spare)
//   5        character speech (mono, so the second of its pair is never opened)
//   6,7      packet voices: the player engine's Ginsu synth (ginsu.h)
//   8..23    one-shot SFX voices
#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

// Mixer buses. Order is ours; the names are the game's own setting keys.
typedef enum {
	AUDIO_BUS_MASTER = 0, // AOMasterVol     -- scales every other bus
	AUDIO_BUS_FE_MUSIC,   // AOFEMusicVol    -- frontend / menu music
	AUDIO_BUS_IG_MUSIC,   // AOIGMusicVol    -- in-race music
	AUDIO_BUS_SFX,        // AOSFXMasterVol  -- UI clicks, world one-shots
	AUDIO_BUS_ENGINE,     // AOEngineVol
	AUDIO_BUS_TIRE,       // AOTireVol
	AUDIO_BUS_SPEECH,     // AOSpeechVol
	// The mix map's own (mixmap.h): a voice whose gain the mix map computed
	// already carries the master and per-kind sliders (SFXCTL_MasterVol), so
	// this bus applies neither.
	AUDIO_BUS_MIXMAP,
	AUDIO_BUS_COUNT
} AudioBus;

// A streaming music slot. Two exist so one can fade in while the other fades
// out (the frontend does this when a race starts).
typedef enum {
	AUDIO_MUSIC_A = 0,
	AUDIO_MUSIC_B,
	// Character speech (speech.h). A spoken line is a one-off ten-second
	// stream rather than a one-shot, so it belongs on the streaming path and
	// not in the SFX bank, and it needs a slot of its own so that starting a
	// line never disturbs the FE->IG music handoff. Mono, so it opens one
	// channel; retail gives speech its own SFXOBJ and its own bus too
	// (SpeechModule::GenStreamVol, Speed.elf 0x800899C4).
	AUDIO_MUSIC_SPEECH,
	AUDIO_MUSIC_SLOTS
} AudioMusicSlot;

// Brings up ndsp and the streaming thread. Safe to call when the DSP firmware
// is unavailable: it returns false, and every other entry point in this header
// then becomes a no-op that reports "not playing", so callers need no guards.
bool audioInit(void);
void audioExit(void);

// True when the DSP came up and audio is actually available.
bool audioAvailable(void);

// Bus gains, 0.0 .. 1.0. Applied to already-playing voices immediately.
void  audioSetBusVolume(AudioBus bus, float gain);
float audioGetBusVolume(AudioBus bus);

// The Audio Mode option: the DSP's output folded to mono, or left stereo.
void audioSetMono(bool mono);

// ---------------------------------------------------------------- music

// Starts streaming a .dspm file into `slot` on `bus`. Any stream already in
// that slot is stopped first. `loop` restarts from the file's loop points (or
// from the beginning when it carries none).
//
// It only asks (#342): the streaming thread opens the file and primes the
// buffers, a few tens of ms later, so the caller's frame never waits on the
// card. Until then the slot counts as playing (audioMusicIsPlaying), and a
// gain, fade or pause set on it holds for the stream when it starts. A file
// that turns out missing or malformed leaves the slot finished with
// audioMusicFailed() true. Returns false only when nothing could be asked:
// no DSP, a bad slot, an empty or over-long path.
bool audioMusicPlay(AudioMusicSlot slot, const char* path, AudioBus bus, bool loop);
// True while the slot's start is still being opened.
bool audioMusicStarting(AudioMusicSlot slot);
// True when the slot's last start found no file, or a bad one.
bool audioMusicFailed(AudioMusicSlot slot);
// The slot's last start, once done: its steps in ms on the streaming thread
// ("open .., prime ..; playing .. ms after the ask"), or why it failed.
const char* audioMusicStartBreakdown(AudioMusicSlot slot);
void audioMusicStop(AudioMusicSlot slot);
bool audioMusicIsPlaying(AudioMusicSlot slot);

// Holds `slot` where it is, or lets it carry on from there -- the PATH_pause
// retail's music controller does for the pause menu (EAXS_PFController::
// SetGameState state 5 / 7, GC 0x80085D5C). A new audioMusicPlay() on the slot,
// or a stop, starts it unpaused again.
void audioMusicPause(AudioMusicSlot slot, bool paused);
bool audioMusicIsPaused(AudioMusicSlot slot);

// Per-slot gain on top of the bus gain, for fades. 0.0 .. 1.0.
void  audioMusicSetGain(AudioMusicSlot slot, float gain);
float audioMusicGetGain(AudioMusicSlot slot);

// Fades `slot` to `target` over `seconds`, driven by the streaming thread.
void audioMusicFadeTo(AudioMusicSlot slot, float target, float seconds);

// Dialogue ducking. Both music buses are held down for as long as a line is
// playing on AUDIO_MUSIC_SPEECH, and come back up when it ends. It is driven
// by the mixer itself rather than by the caller, so starting a line is all
// anyone has to do and a line cut short can never leave the music quiet
// (audio.c has the retail note and the times).
//
// The level is where music sits while a voice plays, 0.0 .. 1.0. It rides on
// top of the bus gain, so the player's Music Volume setting still means what
// it says.
void  audioSetMusicDuckLevel(float gain);

// The factor the music is currently scaled by: 1.0 when nothing is speaking.
float audioMusicDuck(void);

// The in-game music's level from the world's mix map: SFXObj_Music's Q15
// output, which EAXS_PFController::EAXS_UpdateMusic (Speed.elf 0x80086570)
// hands the EA Trax stream as SndStrmWrapper::SetVol(q15 >> 8) of 127. It has
// the master and music sliders and retail's speech duck in it, so while it is
// set it replaces AUDIO_BUS_IG_MUSIC's bus gain and the duck above. Below 0
// hands the music back to the bus (no world, no map).
void audioSetMusicMapGain(float gain);

// Playback position and length in seconds. Both return 0 when idle.
float audioMusicPosition(AudioMusicSlot slot);
float audioMusicDuration(AudioMusicSlot slot);

// True once a non-looping stream has played its last block and drained. This
// is the signal EA Trax uses to advance to the next track.
bool audioMusicFinished(AudioMusicSlot slot);

// ---------------------------------------------------------------- cutscenes

// The boot-movie / cutscene voice is NDSP channel 0, and the video path in
// main.c owns its own PCM buffer rather than going through audioSfxPlay. This
// silences that channel and drops anything still queued on it.
//
// Call it BEFORE freeing the PCM buffer the waveBuf points at: a queued
// waveBuf keeps the DSP reading that memory, so freeing first both leaves the
// skipped clip audible and hands the DSP a freed linear-memory block.
#define AUDIO_CH_CUTSCENE 0
void audioCutsceneStop(void);

// Sets the movie voice up for a clip of `channels` (1 or 2) PCM16 at `rate`,
// at retail's movie level: RCMP_Manager::SetMovieCategoryVolume (Speed PS2
// 0x80119B44) is the movie entry's volume times FEDatabase's Master and
// "Speech & Movie" volumes, so here the AUDIO_BUS_SPEECH bus gain (which has
// the master in it). audioSetBusVolume keeps it current while a movie plays.
// PORT: the per-movie entry volume (fe_movie_entries +0x1F, GetMovieVolume
// 0x8014D288) is not read; every movie plays at the entry's full level.
void audioCutsceneStart(uint32_t rate, int channels);
// Queues one buffer of the clip, and whether the voice is still sounding.
void audioCutsceneQueue(ndspWaveBuf* wb);
bool audioCutscenePlaying(void);

// ---------------------------------------------------------------- sfx

// Fire-and-forget PCM16 one-shot from a RIFF/WAVE file, mixed on the DSP.
// Returns the NDSP channel used, or -1 if no voice was free. `pan` is -1.0
// (hard left) .. 1.0 (hard right).
int  audioSfxPlay(const char* path, AudioBus bus, float gain, float pan);
void audioSfxStopAll(void);

// ------------------------------------------------- frontend sound banks
//
// Retail's two, both built by tools/fesfx_build.py. EAXSound::PlayUISoundFX
// (0x8007AC9C) routes eMenuSoundTriggers 0..0x16 to EAXCommon and 0x17..0x5D
// to EAXFrontEnd:
//   - AUDIO_FEBANK_COMMON, audio/FESFX.BIN: SOUND/GLOBAL/FE_COMMON_MB.abk, the
//     menu clicks and scrolls, about 120 KB;
//   - AUDIO_FEBANK_FRONTEND, audio/FEMB.BIN: SOUND/FE/FE_MB.abk, the sounds
//     the garage, the shops and the customiser make, about 910 KB.
// Every clip is one DSPADPCM block held in linear memory, so a sound costs one
// waveBuf submit and no file I/O.
//
// Both files are optional on the card. Without one its load returns false, its
// count is 0, and audioSfxBankPlay() on it is a no-op: those sounds stay silent.
typedef enum {
	AUDIO_FEBANK_COMMON,
	AUDIO_FEBANK_FRONTEND,
	AUDIO_FEBANK_COUNT
} AudioFeBank;

bool audioSfxBankLoad(AudioFeBank bank, const char* path);
void audioSfxBankFree(AudioFeBank bank);
int  audioSfxBankCount(AudioFeBank bank);
int  audioSfxBankPlay(AudioFeBank bank, int index, AudioBus bus, float gain, float pan);

// ------------------------------------------------------ packet voices
//
// NDSP channels 6 and 7: a mono PCM16 voice fed one packet at a time -- the
// 3DS side of SND's packet player (SNDPKTPLAY), which the Ginsu engine synth
// plays through. `fill` runs on the packet thread whenever a packet has played:
// it writes the next one (at most `capacity` samples) and returns its length,
// and must not call back into this API. Start queues silent packets of
// `silentLen` first. The mix is gain * pan per side on `bus`, as an AEMS voice.
#define AUDIO_PACKET_VOICES 2
typedef int (*AudioPacketFill)(void* user, int16_t* out);
bool audioPacketStart(int voice, uint32_t rate, int capacity, int silentLen,
                      AudioPacketFill fill, void* user);
void audioPacketStop(int voice);
void audioPacketSetRate(int voice, float rate);
void audioPacketSetMix(int voice, int bus, float gain, float panLeft, float panRight);
// SNDCTRL_lowpass: the voice's cutoff in Hz, through NDSP's one-pole filter;
// anything from 16 kHz up leaves it open.
void audioPacketSetLowpass(int voice, float hz);
void audioPacketPause(int voice, bool paused);

// Debug: what NDSP channel `ch` is doing (rate, front mix, position), read
// back from NDSP itself. 0 and nothing written when the channel is silent.
int  audioDebugChannel(int ch, char* out, size_t n);

// Debug: the output stage since the last call (#310): the loudest summed
// front gain, the lowest master volume it set, fades running, and voice
// starts refused for want of a free channel.
int  audioDebugMix(char* out, size_t n);

// Debug: the DSP's side since the last call (#310): frames run and dropped
// (libctru's count, from the DSP), the frame callback's longest run and the
// longest gap between two, the packet thread's slowest answer to a wake, and
// the fewest packets an engine voice had left when refilled (0 = ran dry).
int  audioDebugDsp(char* out, size_t n);

// Pumps fades and reclaims finished SFX voices. Streaming does not depend on
// this -- it runs on its own thread -- but calling it once a frame keeps the
// voice pool tidy. Cheap and safe to call when audio is unavailable.
void audioUpdate(void);
