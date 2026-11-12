// NFSU2-3DS character speech.
//
// Retail's speech is a whole subsystem: UG2_SpeechParser hands a Csis call to
// SPCH, which matches it against an event dat, picks a sample out of a loaded
// voice bank and streams it through a SpeechModule (Speed.elf 0x8008CA34 and
// 0x802EF63C onwards, all of it still carrying C++ symbols). None of that
// machinery is ported, and none of it needs to be: the match is deterministic
// for a scripted line, so it is done once on the host and the console just
// plays a file.
//
// tools/speech_build.py carries the decode in full. The short of it:
//
//   * PC Files/speech/UG2_Speech.big is 857 EA MicroTalk streams, 22050 Hz
//     mono, each starting on a 0x100-byte boundary.
//   * UG2_Speech.idx is the clump index -- fourteen groups, each a 12-byte
//     header and then one five-byte record per sound: a big-endian u16 sector
//     offset from the group's base, then three Csis parameters.
//   * The parameters are stored as BIT INDICES, because Csis passes its enums
//     as one-hot flags. CareerSpeech::Stage0Intro (0x8018A8B4) builds
//     SpeechCmdCareer(0x8000, 0x20), and the index holds 15 and 5. The widths
//     come from the .evt's own filter masks, 0x1FFFFFFF and 0x3FF.
//   * A record is 2 + nparm bytes, nparm being the sub-table header's +4.
//     Parameter 0 is the DX_Mode bit the parser call passes first (1 career,
//     2 frontend, 4 timed, 8 Outrun, 0x10 intro-event, 0x20 intro-area).
//   * Group 0 is the Career category, and each of the four Stage 0 lines
//     resolves there to exactly one record -- no alternates, which is what a
//     scripted prologue has to be. Most other calls have several.
//
// Every record is converted to a Nintendo DSPADPCM .dspm, g<group>_<record>
// (g07_012.dspm), and played on the streaming path (audio.h,
// AUDIO_MUSIC_SPEECH on AUDIO_BUS_SPEECH), so the ARM11 never decodes a
// speech sample. The card also carries UG2_Speech.idx itself: speechInit
// parses it, and each call below is looked up there and one of its
// alternates picked, as SPCH does (TASKS.md #268 I).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The prologue's four lines (stage0.c), each a SpeechCmdCareer call.
typedef enum {
	SPEECH_STAGE0_INTRO = 0,  // CareerSpeech::Stage0Intro   CR_Event 0x8000
	SPEECH_STAGE0_SMS,        // CareerSpeech::Stage0SMS     CR_Event 0x800
	SPEECH_STAGE0_MINIMAP,    // CareerSpeech::Stage0Minimap CR_Event 0x2
	SPEECH_STAGE0_CARLOT,     // CareerSpeech::Stage0Carlot  CR_Event 0x10000000
	SPEECH_LINE_COUNT
} SpeechLine;

// Points the player at <dataDir>/speech and parses its UG2_Speech.idx.
// Returns false when there is no idx; every other entry point then no-ops
// safely and speechIsPlaying() is always false, so a card without the speech
// set runs the prologue silently on its scripted timings rather than hanging
// on it.
bool speechInit(const char* dataDir);
void speechExit(void);

// True when the idx was found and a line can actually be looked up.
bool speechAvailable(void);

// Starts `line`, stopping whatever was speaking. Retail's speech is one voice
// at a time per module, so this is a replace and not a mix. Returns false when
// the file is missing or the DSP is not up.
bool speechPlay(SpeechLine line);
void speechStop(void);

// UG2_SpeechParser's Play* calls (SpeedPS.elf 0x8008CB30..0x8008CFFC), with
// the Csis values as retail passes them -- one-hot flags, not bit numbers.
// `purge` is DoSpeechCall's bool (0x801889C8): PurgeSpeech first, so the new
// line cuts the old one off. Without it, a call made while a line is playing
// waits behind it (SPCH's event queue; PORT: four deep here, and nothing
// expires). Each returns false when no record matches or its file is
// missing.
bool speechPlayCareerID(uint32_t crEvent, uint32_t charId, bool purge);
bool speechPlayCareerMag(uint32_t magName, uint32_t status, bool purge);
bool speechPlayCareerDVD(uint32_t dvdName, uint32_t status, bool purge);
bool speechPlayOutrun(uint32_t orParms, uint32_t charId, bool purge);
bool speechPlayTimed(uint32_t timeProgress, bool purge);
bool speechPlayFEScreen(uint32_t feScreen, uint32_t charId, bool purge);
bool speechPlayFEDynoshop(uint32_t dynoshop, uint32_t charId, bool purge);
bool speechPlayIntroArea(uint32_t locations, uint32_t charId, bool purge);
bool speechPlayIntroEvent(uint32_t locations, uint32_t charId, bool purge);

// UG2_SpeechParser::PurgeSpeech (0x8008D094): the line playing and the ones
// waiting are dropped.
void speechPurge(void);

// CareerSpeech::IsSpeechDone (0x8018A9B8) is the negation of this. It is false
// when nothing was ever started, which is what lets a silent card walk the
// prologue through on its timers.
bool speechIsPlaying(void);

// Playback position and length of the current line, in seconds. Both 0 when
// idle. Useful for subtitles later (#267) and for the test log.
float speechPosition(void);
float speechDuration(void);

// ------------------------------------------------------------ the NIS module
//
// SpeechManager's second module (+8), the NIS_AudioParser's (EAXSound +0x28).
// It plays the in-game movies' streams and the one stream the world asks for,
// EAXCollision::QueueCollisionStream (Speed.elf 0x80080264): the crash
// camera's CollisionStream (main.c, TASKS.md #335). tools/nisaudio_build.py
// has the bank's decode; "CollisionStream" is six alternates there, built as
// colstream0..5.dspm in the same speech directory, stereo at 32 kHz.
//
// It has its own stream slot (AUDIO_MUSIC_B), so a crash never cuts a line
// of character speech off, as two SpeechModules never do.

// QueStream(5, "CollisionStream", ...), then the callback's PlayStream: one
// of the six, picked with `rng` (bRandom's state), replacing what the module
// was playing (QueStream's DonePlaying). PORT: SPCH picks the alternate with
// its own random; the port draws from the game's shared bRandom. False when
// no alternate is on the card (silence, nothing else) or the DSP is not up.
bool speechCollisionStream(uint32_t* rng);
// CARSFX_Rain::QueueWeatherStream's QueStream(4, "THUNDERSTRM", variant)
// and PlayWeatherStream's PlayStream (TASKS.md #315): one of thunder0..8.dspm
// (records 14..22, SoundFX_Select {4, 1}). Variant 2 (the heavy rain's,
// {4, 2}) has no sound in the retail bank: false, silent, as there.
bool speechThunderStream(int variant, uint32_t* rng);
// SFXCTL_Physics::UpdateParams (0x8009469C): the human
// car's blown engine queues QueStream(8, "BLOWNENGINE"), which QueStream
// (0x8008B0A0) maps to SoundFX_Select {8, 0x10} -- group 3's records 9..12,
// four alternates, built as blownengine0..3.dspm (#513). Picked with `rng`
// as the collision stream's; false when none is on the card.
bool speechBlownEngineStream(uint32_t* rng);
// The NIS scenes' own streams (#497): NIS_AudioParser::QueStream's answer to
// CAnimPlayer::Load (QueStream(2, scene hash, camera track)), played from the
// scene's start (CAnimScene::Play -> SFXObj_NISStream::StartNIS, PlayStream
// 2). `stem` names the file tools/nisaudio_build.py's scene_records() built
// for the Csis parameters: nisend_<p0>_<p1> (NIS_Select_End: the race-end,
// shop, dyno and career scenes), nisstart_<p0>_<p1>_<p2> (NIS_Select_Start:
// the race-start crowd, p2 = 2 the button-through take). Replaces what the
// module was playing; false (silent) when the card has no such file -- the
// asks retail's bank does not answer too ("NO MATCHING NIS HASHID").
bool speechNisScene(const char* stem);
void speechNisStop(void);
bool speechNisPlaying(void);

// Each world frame: SpeechManager::PauseSpeech / ResumeSpeech (0x800895E8),
// which pause both modules under the pause menu, and the NIS stream's level,
// SpeechModule::GenStreamVol (0x800899C4) for module 5 -- SFXObj_Speech's
// Q15 output 3 (mixmap.h), 0..32767. Below 0 (no mix map) the stream rides
// the speech bus at full gain instead.
void speechUpdate(bool paused, int32_t nisQ15);
