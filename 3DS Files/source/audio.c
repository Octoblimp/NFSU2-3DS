// NFSU2-3DS hardware audio mixer -- see audio.h for the design and bus layout.
//
// Music lives on the SD card as Nintendo DSPADPCM (4 bits/sample). The
// streaming thread below reads it block by block, expands it to PCM16 and
// hands NDSP one channel per stream (#310: the DSP's own ADPCM decode and
// polyphase filter were the EA Trax's crackle on the 3DS -- see musicDecode).
// Resampling and mixing stay on the DSP. The effects stay DSPADPCM on the DSP.

#include "audio.h"
#include "aems.h"
#include "dbglog.h"

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------- .dspm

// Written by tools/src/dspenc.c; keep the two in lockstep. Field order and
// sizes are fixed, so this maps straight onto the first 0x28 bytes of the file.
typedef struct {
	char magic[4];       // "DSPM"
	u32  version;        // 1
	u32  sampleRate;
	u32  channels;       // 1 or 2
	u32  totalSamples;   // per channel
	u32  framesPerBlock;
	u32  blockCount;
	u32  dataOffset;
	u32  loopStart;      // samples, 0 = none
	u32  loopEnd;
} DspmHeader;

#define DSPM_FRAME_SAMPLES 14
#define DSPM_FRAME_BYTES    8
#define MUSIC_BUFFERS       4   // ~900 ms of slack at the default 512-frame block
#define MUSIC_PATH_MAX    256   // sdmc:/nfsu2data/... is well under it

// NDSP channel assignment (see audio.h).
#define CH_MUSIC_BASE  1        // stream A -> 1   stream B -> 3   speech -> 5
#define CH_SFX_FIRST   8
#define CH_SFX_LAST   23
// The SFX channels: 8..23 and the music pairs' spare 2 and 4 (a stereo
// PCM16 stream opens one channel, so nothing uses them).
#define SFX_CHANNELS  (CH_SFX_LAST - CH_SFX_FIRST + 1 + 2)
// Voices, more than channels: an AEMS loop that has gone silent can give its
// channel up and play on without one (sfxChanFind, audioUpdate).
#define SFX_VOICES    48

#define AUDIO_HEADROOM 0.8f     // NDSP master volume, about -2 dB (audioInit)

typedef struct {
	bool   active;
	FILE*  fp;
	DspmHeader hdr;

	int    chan;                    // the stream's NDSP channel, PCM16 mono or stereo
	u16    coefs[2][16];
	ndspAdpcmData* index;           // blockCount * channels, [block][channel]

	u8*    raw;                     // one DSPADPCM block as read off the card
	s16*   pcm[MUSIC_BUFFERS];      // linear memory, one decoded block each
	ndspWaveBuf wb[MUSIC_BUFFERS];

	u32    blockBytes;              // channels * framesPerBlock * 8
	u32    totalFrames;             // ceil(totalSamples / 14)
	u32    nextBlock;               // next block to submit
	u32    blocksPlayed;
	u32    loopBlock;

	bool   loop;
	bool   drained;                 // every block submitted and played out
	bool   paused;                  // held by audioMusicPause()
	AudioBus bus;

	float  gain;
	float  fadeTarget;
	float  fadeRate;                // gain units per second, 0 = no fade

	// A start audioMusicPlay asked for that the streaming thread has not
	// opened yet (#342, musicStartPending). Read without the lock by the
	// queries, so it is set and cleared with release stores, last.
	bool   pending;
	bool   failed;                  // the last start found no file, or a bad one
	u32    reqGen;                  // bumped by every Play and Stop on the slot
	u64    reqTick;                 // when it was asked for
	char   reqPath[MUSIC_PATH_MAX];
	char   why[200];                // the last start's steps (audioMusicStartBreakdown)
} MusicStream;

typedef struct {
	bool  active;
	int   chan;                     // its NDSP channel; -1 while an AEMS loop is virtual
	s16*  pcm;                      // owned PCM16, freed on reclaim; NULL for bank clips
	ndspAdpcmData adp;              // the clip's start context, copied per voice
	ndspWaveBuf wb;

	// An AEMS voice (aems.h) belongs to a sound graph's player node, which
	// steers it by handle for as long as it plays.
	bool  aems;
	int   bank;                     // the AudioFeBank a bank clip is from
	int   gen;                      // the handle's upper bits; a reused voice gets a new one
	const AemsClip* clip;
	AudioBus bus;
	float pitch, gain, panL, panR;
	bool  paused;                   // aemsVoicePause, kept for a channel taken back
	s8    fadeChan;                 // the channel its stop is fading out on, which
	                                // still reads `wb` (#351); -1 none
	u8    orphan;                   // frames no graph has held it (#351)
} SfxVoice;

// ---------------------------------------------------------------- FESFX bank

// Written by tools/fesfx_build.py; keep the two in lockstep.
typedef struct {
	char magic[4];       // "FSFX"
	u32  version;        // 1
	u32  count;
	u32  rate;           // informational -- each clip carries its own
} SfxBankHeader;

// One menu sound: a single DSPADPCM block, so one waveBuf plays the whole clip.
typedef struct {
	u8*  data;           // linear memory
	u32  bytes;
	u32  samples;
	u32  rate;
	u16  coefs[16];
	ndspAdpcmData start;
} SfxClip;

#define SFX_BANK_MAX 72   // FE_MB.abk has 68

static bool        s_ready;
// Two locks, deliberately. The streaming thread holds s_musicLock across its
// SD reads, and audioUpdate() runs on the render thread every frame -- sharing
// one lock made the render thread block on file I/O from a higher-priority
// thread, which starved input polling badly enough to wedge the title screen.
static LightLock   s_musicLock;
static LightLock   s_sfxLock;
static Thread      s_thread;
static volatile bool s_threadRun;
// Wakes the streaming thread before its tick is up, for a start (#342).
static LightEvent  s_streamWake;

static MusicStream s_music[AUDIO_MUSIC_SLOTS];
// Debug: the fewest blocks a playing stream had queued when the pump came
// round, since audioDebugMix(). 0 = the DSP ran dry.
static volatile int s_dbgMusicLow = MUSIC_BUFFERS;
static SfxVoice    s_sfx[SFX_VOICES];
static SfxClip     s_clip[AUDIO_FEBANK_COUNT][SFX_BANK_MAX];
static int         s_clipCount[AUDIO_FEBANK_COUNT];
static float       s_bus[AUDIO_BUS_COUNT];

// Dialogue ducking. Retail does this in EA's AEMS sound script rather than in
// code -- all Speed.elf keeps of it is the pair of script variable names
// `Env_Duck` (0x803A3EA0) and `IntensityVsDucking` (0x803A3DBC), both .bss fed
// from the sound bank's own data, which this port does not parse. So the
// amount and the two times below are the port's own, not recovered constants.
//
// It is driven from the speech slot here and not by the game, for the same
// reason retail puts it in the sound script: it is a property of the mix, so
// every caller that ever starts a line gets it without having to remember to,
// and nothing can leave the music held down after a line is cut short, which
// is exactly what a flag set by gameplay code would eventually do.
//
// The factor rides on top of the bus gain rather than replacing it, so the
// player's Music Volume setting still means what it says and the duck never
// has to remember what the volume was before it.
//
// In the world, retail's own duck is in the mix map after all: MAPOUTPUT.mxb's
// Main state sub channel 1 has envelopes keyed on SFXObj_Speech's output 1
// (-13.2 dB and others) on the way to SFXObj_Music. While the map drives the
// music (audioSetMusicMapGain), that is the duck and this one stands aside;
// it stays for the frontend, which runs no map.
#define DUCK_ATTACK  0.15f   // s, down -- fast enough to be under the first word
#define DUCK_RELEASE 0.50f   // s, back up -- slow, so it is not a pop

static float       s_duckLevel = 0.30f;  // where music sits while a voice plays
static float       s_duck = 1.0f;
static float       s_duckTarget = 1.0f;
static float       s_duckRate;           // gain units per second, 0 = settled

// The in-game music's level off the mix map (audioSetMusicMapGain), < 0 when
// no map drives it.
static float       s_musicMap = -1.0f;

// ---------------------------------------------------------------- helpers

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// The slider gain a bus puts on a voice: its own times the master's. The mix
// map's voices have both in their gain already.
static float busGain(int bus)
{
	if (bus == AUDIO_BUS_MIXMAP) return 1.0f;
	return clamp01(s_bus[bus]) * clamp01(s_bus[AUDIO_BUS_MASTER]);
}

// ---------------------------------------------------------------- output stage
//
// The port's, not retail's (#310). On the 3DS the DSP takes a channel's new
// mix at its next 160-sample frame as a step, and a channel cleared mid-wave
// drops to zero in one sample. A game frame there is 20-50 ms, so a skid's or
// a scrape's gain moved in big steps, and every skid layer swap and scrape
// stop cut a voice dead: dozens of clicks a second in a drift or along a
// wall, which the 3DS's speaker turns into static. And a wall hit stacks the
// impact, the scrape and the skids on the engine and the music past full
// scale, into the DSP's clipper. So every mix the port sets goes through here:
//  * a running voice's gain glides to its new value, a step per DSP frame
//    (mixPump(), from the frame callback, every ~4.9 ms);
//  * a stopped AEMS voice fades out over MIX_FADE_FRAMES, and its channel is
//    cleared, and free again, only then;
//  * the master volume comes down at once when what is sounding could sum
//    past full scale (every source counted at full scale: the EA Trax and
//    Ginsu's engines both reach it), and goes back up slowly.

#define MIX_CHANNELS    24
#define MIX_GLIDE       0.3f    // of the way to the new gain per DSP frame, ~14 ms
#define MIX_SETTLE      0.0005f
#define MIX_FADE_FRAMES 6       // ~29 ms
#define MIX_CEILING     1.0f    // the summed front gain the master keeps under
#define MIX_RELEASE     0.01f   // of the way back up per DSP frame, ~0.5 s

enum { MIX_IDLE, MIX_ON, MIX_FADING };

typedef struct {
	float tgt[2], cur[2];   // front left / right
	u8    state;
	u8    fade;             // frames left in a fade
} MixChan;

static MixChan   s_mix[MIX_CHANNELS];
static LightLock s_mixLock;
static float     s_master = AUDIO_HEADROOM, s_masterSent = AUDIO_HEADROOM;
static float     s_dbgLoad, s_dbgMasterLow = AUDIO_HEADROOM;   // since audioDebugMix()
static int       s_dbgRefused;                                  // starts with no voice free
static int       s_dbgStolen;                                   // channels taken off a quieter voice

// Caller holds s_mixLock.
static void mixWrite(int ch, const float g[2])
{
	float mix[12];
	memset(mix, 0, sizeof(mix));
	mix[0] = g[0];   // front pair only, as buildMix
	mix[1] = g[1];
	ndspChnSetMix(ch, mix);
}

// A voice's mix from now on: at once if the channel is starting, gliding if
// it is already sounding.
static void mixSet(int ch, float l, float r)
{
	if (ch < 0 || ch >= MIX_CHANNELS) return;
	LightLock_Lock(&s_mixLock);
	MixChan* m = &s_mix[ch];
	if (m->state == MIX_FADING) { LightLock_Unlock(&s_mixLock); return; }
	m->tgt[0] = l; m->tgt[1] = r;
	if (m->state == MIX_IDLE) {
		m->cur[0] = l; m->cur[1] = r;
		m->state = MIX_ON;
		mixWrite(ch, m->cur);
	}
	LightLock_Unlock(&s_mixLock);
}

// The channel has been (or is about to be) reset by its owner: forget it.
static void mixCut(int ch)
{
	if (ch < 0 || ch >= MIX_CHANNELS) return;
	LightLock_Lock(&s_mixLock);
	MixChan* m = &s_mix[ch];
	m->tgt[0] = m->tgt[1] = m->cur[0] = m->cur[1] = 0.0f;
	m->state = MIX_IDLE;
	LightLock_Unlock(&s_mixLock);
}

// Fades the channel out; mixPump() clears and resets it when the fade ends.
static void mixFadeStop(int ch)
{
	if (ch < 0 || ch >= MIX_CHANNELS) return;
	LightLock_Lock(&s_mixLock);
	MixChan* m = &s_mix[ch];
	if (m->state == MIX_ON) {
		m->tgt[0] = m->tgt[1] = 0.0f;
		m->fade = MIX_FADE_FRAMES;
		m->state = MIX_FADING;
	} else if (m->state == MIX_IDLE) {
		ndspChnWaveBufClear(ch);
		ndspChnReset(ch);
	}
	LightLock_Unlock(&s_mixLock);
}

static bool mixFading(int ch)
{
	return ch >= 0 && ch < MIX_CHANNELS && s_mix[ch].state == MIX_FADING;
}

// Ends the channel's fade now, in a cut.
static void mixCutFade(int ch)
{
	if (ch < 0 || ch >= MIX_CHANNELS) return;
	LightLock_Lock(&s_mixLock);
	MixChan* m = &s_mix[ch];
	if (m->state == MIX_FADING) {
		ndspChnWaveBufClear(ch);
		ndspChnReset(ch);
		m->cur[0] = m->cur[1] = 0.0f;
		m->state = MIX_IDLE;
	}
	LightLock_Unlock(&s_mixLock);
}

// Ends every fade now, for an owner about to free what the channels read.
static void mixCutFades(void)
{
	LightLock_Lock(&s_mixLock);
	for (int ch = 0; ch < MIX_CHANNELS; ch++) {
		MixChan* m = &s_mix[ch];
		if (m->state != MIX_FADING) continue;
		ndspChnWaveBufClear(ch);
		ndspChnReset(ch);
		m->cur[0] = m->cur[1] = 0.0f;
		m->state = MIX_IDLE;
	}
	LightLock_Unlock(&s_mixLock);
}

// One DSP frame of the output stage, on the DSP thread. A busy lock waits for
// the next frame rather than hold the DSP thread up.
static void mixPump(void)
{
	if (LightLock_TryLock(&s_mixLock) != 0) return;
	float load[2] = { 0.0f, 0.0f };
	for (int ch = 0; ch < MIX_CHANNELS; ch++) {
		MixChan* m = &s_mix[ch];
		if (m->state == MIX_IDLE) continue;
		if (m->state == MIX_FADING) {
			if (m->fade == 0) {
				ndspChnWaveBufClear(ch);
				ndspChnReset(ch);
				m->cur[0] = m->cur[1] = 0.0f;
				__sync_synchronize();
				m->state = MIX_IDLE;
				continue;
			}
			const float k = (float)(m->fade - 1) / (float)m->fade;
			m->fade--;
			m->cur[0] *= k; m->cur[1] *= k;
			mixWrite(ch, m->cur);
		} else if (m->cur[0] != m->tgt[0] || m->cur[1] != m->tgt[1]) {
			for (int s = 0; s < 2; s++) {
				const float d = m->tgt[s] - m->cur[s];
				m->cur[s] = fabsf(d) < MIX_SETTLE ? m->tgt[s] : m->cur[s] + d * MIX_GLIDE;
			}
			mixWrite(ch, m->cur);
		}
		if (ndspChnIsPlaying(ch) && !ndspChnIsPaused(ch)) {
			load[0] += m->cur[0];
			load[1] += m->cur[1];
		}
	}
	const float l = load[0] > load[1] ? load[0] : load[1];
	float want = AUDIO_HEADROOM;
	if (l * want > MIX_CEILING) want = MIX_CEILING / l;
	if (want < s_master) s_master = want;
	else                 s_master += (want - s_master) * MIX_RELEASE;
	if (fabsf(s_master - s_masterSent) >= 0.001f) {
		ndspSetMasterVol(s_master);
		s_masterSent = s_master;
	}
	if (l > s_dbgLoad) s_dbgLoad = l;
	if (s_master < s_dbgMasterLow) s_dbgMasterLow = s_master;
	LightLock_Unlock(&s_mixLock);
}

// Fills a 12-entry NDSP mix for one source channel. `pan` -1 = hard left,
// 0 = centre, +1 = hard right.
//
// Front pair only (entries 0/1). In NDSP_OUTPUT_STEREO the DSP folds the back
// pair into the front one (left = front left + back left), so a sound written
// to both pairs comes out at twice its level. That was this port's static: the
// EA Trax masters peak at 0 dBFS with a tenth of their samples above half
// scale, and doubled, every one of those clipped. NFSHP2-3DS (bsnd.c hw_mix,
// jbstream.c) has always set the front pair alone, on Azahar and hardware.
static void buildMix(float mix[12], float gain, float pan)
{
	float l = gain * (pan <= 0.0f ? 1.0f : 1.0f - pan);
	float r = gain * (pan >= 0.0f ? 1.0f : 1.0f + pan);
	memset(mix, 0, sizeof(float) * 12);
	mix[0] = l; mix[1] = r;
}

static void aemsApplyMix(SfxVoice* v);
static void packetBusChanged(void);

// ------------------------------------------------- SFX voices and channels
//
// The port's voice manager, not retail's. Retail's AEMS had the GameCube's
// 64 AX voices (and the PC DirectSound's) and stole by priority; the 3DS has
// 24 NDSP channels, 18 of them for effects. Three opponents' engines alone
// hold a dozen AEMS loops, out of earshot most of a race, and with a channel
// each they left the player's own sounds none (#298: 21 starts refused in
// five seconds). So a voice is a record, and a channel goes to the loudest:
//
//   - A voice starts on a free channel, else on the most faded of the ones
//     fading out (as every stop was before #310), else on the channel of the
//     quietest AEMS voice under half its own level, which it takes: a loop
//     gives it up and plays on virtual, a one-shot ends.
//   - An AEMS loop with no channel starts virtual: alive to its graph, which
//     steers it as ever, but silent and holding nothing.
//   - Each frame audioUpdate gives the loudest virtual loops that are no
//     longer silent a channel the same way, from the top of the clip -- they
//     come back from silence, so the jump is not heard.
//   - Anything else that finds no channel is refused, as before.

#define SFX_SILENT 0.0005f   // a voice's level at or under this is not heard (-66 dB)
#define SFX_STEAL  0.5f      // a channel is taken only off a voice under this much of the taker

static const u8 s_sfxChan[SFX_CHANNELS] = {
	8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 2, 4 };
static s8 s_chanVoice[MIX_CHANNELS];   // the voice on each channel, -1 none

static bool sfxChanIs(int ch)
{
	for (int k = 0; k < SFX_CHANNELS; k++)
		if (s_sfxChan[k] == ch) return true;
	return false;
}

// How loud the voice is now, 0..1: an AEMS voice's gain on its bus at its
// louder side. A menu one-shot is 1, never given up.
static float sfxLevel(const SfxVoice* v)
{
	if (!v->aems) return 1.0f;
	const float p = clamp01(v->panL) > clamp01(v->panR) ? clamp01(v->panL) : clamp01(v->panR);
	return clamp01(v->gain) * busGain(v->bus) * p;
}

// A free voice record's index, or -1. Caller holds s_sfxLock.
//
// #351: a record whose stop is still fading out is the last choice, and its
// fade is cut first. Its channel reads the record's `wb` until the fade ends,
// and set up for a new clip, the same wave buffer would be on two channels'
// queues at once.
static int sfxVoiceFree(void)
{
	int fading = -1;
	for (int i = 0; i < SFX_VOICES; i++) {
		if (s_sfx[i].active) continue;
		if (s_sfx[i].fadeChan >= 0 && mixFading(s_sfx[i].fadeChan)) {
			if (fading < 0) fading = i;
			continue;
		}
		s_sfx[i].fadeChan = -1;
		return i;
	}
	if (fading >= 0) {
		mixCutFade(s_sfx[fading].fadeChan);
		s_sfx[fading].fadeChan = -1;
	}
	return fading;
}

// Takes the voice off its channel now: a loop goes virtual, anything else
// ends. Caller holds s_sfxLock.
//
// #330: a loop whose wave buffer already reads DONE is dead -- its player has
// seen aemsVoiceAlive() false and dropped the handle -- so it ends as well.
// Sent virtual it was an active voice nobody held, which came back on a
// channel after its bank was freed (the 11-02 data abort).
static void sfxChanDrop(SfxVoice* v)
{
	const int ch = v->chan;
	if (ch < 0) return;
	const bool dead = v->wb.status == NDSP_WBUF_DONE;
	ndspChnWaveBufClear(ch);
	ndspChnReset(ch);
	mixCut(ch);
	s_chanVoice[ch] = -1;
	v->chan = -1;
	if (v->aems && v->clip && v->clip->loop && !dead) return;
	if (v->pcm) { linearFree(v->pcm); v->pcm = NULL; }
	v->active = false;
	v->aems = false;
}

// A channel for a voice at `level`, free for it to set up, or -1 (see above).
// Caller holds s_sfxLock.
static int sfxChanFind(float level)
{
	int fading = -1, victim = -1;
	float least = 2.0f, quietest = level * SFX_STEAL;
	for (int k = 0; k < SFX_CHANNELS; k++) {
		const int ch = s_sfxChan[k];
		const int vi = s_chanVoice[ch];
		if (vi < 0) {
			if (!mixFading(ch)) return ch;
			const float g = s_mix[ch].cur[0] > s_mix[ch].cur[1] ? s_mix[ch].cur[0] : s_mix[ch].cur[1];
			if (g < least) { least = g; fading = ch; }
			continue;
		}
		if (!s_sfx[vi].aems) continue;
		const float g = sfxLevel(&s_sfx[vi]);
		if (g < quietest) { quietest = g; victim = ch; }
	}
	if (fading >= 0) {
		mixCutFade(fading);
		return fading;
	}
	if (victim >= 0) {
		sfxChanDrop(&s_sfx[s_chanVoice[victim]]);
		s_dbgStolen++;
		return victim;
	}
	return -1;
}

// Puts voice `i` on channel `ch`. Caller holds s_sfxLock.
static void sfxChanGive(int i, int ch)
{
	s_sfx[i].chan = ch;
	s_chanVoice[ch] = (s8)i;
}

// A voice record with a channel for a sound at `level`, for the caller to set
// up and mark active, or NULL: refused. Caller holds s_sfxLock.
static SfxVoice* sfxVoiceTake(float level)
{
	const int i = sfxVoiceFree();
	const int ch = i >= 0 ? sfxChanFind(level) : -1;
	if (ch < 0) { s_dbgRefused++; return NULL; }
	sfxChanGive(i, ch);
	return &s_sfx[i];
}

static bool busIsMusic(AudioBus bus)
{
	return bus == AUDIO_BUS_FE_MUSIC || bus == AUDIO_BUS_IG_MUSIC;
}

// #491 (the user: "Equalize the music ... just equalize it everywhere"): the
// world's mix map plays SFXObj_Music at 0x2D41 of 0x7FFF (-9 dB) under the
// sliders when nothing ducks it ("MIXMAP music: slots 23fd / 2d41"), so music
// the map does not drive -- the frontend's, and the in-game stream before the
// map's first level -- sits at that same level instead of the sliders' full
// one (the frontend was about 9 dB louder, and the world's first second too).
#define MUSIC_UNMAPPED_LEVEL (0x2D41 / 32767.0f)

static float musicEffectiveGain(const MusicStream* s)
{
	// The map's level already has the sliders and retail's own speech duck.
	if (s->bus == AUDIO_BUS_IG_MUSIC && s_musicMap >= 0.0f)
		return clamp01(s->gain) * clamp01(s_musicMap);
	float g = clamp01(s->gain) * busGain(s->bus);
	if (busIsMusic(s->bus))
		g *= clamp01(s_duck) * MUSIC_UNMAPPED_LEVEL;
	return g;
}

static void musicApplyMix(MusicStream* s)
{
	// One channel either way. A stereo PCM16 channel takes its left samples to
	// the left output at mix[0] and its right ones to the right at mix[1].
	float g = musicEffectiveGain(s);
	mixSet(s->chan, g, g);
}

// The file and the buffers, and nothing of the DSP's. Also used on a start
// being built off to one side (musicPrepare), which owns no channel yet.
static void musicRelease(MusicStream* s)
{
	for (int i = 0; i < MUSIC_BUFFERS; i++) {
		if (s->pcm[i]) { linearFree(s->pcm[i]); s->pcm[i] = NULL; }
	}
	free(s->raw); s->raw = NULL;
	free(s->index); s->index = NULL;
	if (s->fp) { fclose(s->fp); s->fp = NULL; }
}

// Releases every resource a stream holds. Caller must hold the lock.
static void musicTeardown(MusicStream* s)
{
	if (!s->active) return;
	ndspChnWaveBufClear(s->chan);
	ndspChnReset(s->chan);
	ndspChnSetPaused(s->chan, false);
	mixCut(s->chan);
	s->paused = false;
	musicRelease(s);
	s->active = false;
	s->drained = true;
}

// One channel of one block, DSPADPCM to PCM16, into every `step`th sample of
// `out`. Nintendo's decode: the history from the block's index record, the
// predictor and scale from each frame's header byte, saturated at s16.
//
// On the ARM11 and not the DSP (#310). The EA Trax masters sit at 0 dBFS, and
// their ADPCM runs past s16 tens of times a second on a loud track (track00's
// left channel: 21365 samples in 374 s, by up to ~2000), and between samples
// a band-limited reconstruction goes up to ~24% past full scale. On
// the 3DS that crackled whatever the volume; Azahar saturates its decode and
// plays NDSP_INTERP_POLYPHASE as linear, so it never did. NFSHP2-3DS streams
// its EA Trax the same way as this -- decoded on the ARM11, PCM16, linear --
// and plays clean at full volume. A block costs well under a millisecond.
static void musicDecode(const u8* src, u32 frames, const u16 coefs[16],
                        const ndspAdpcmData* ctx, s16* out, int step)
{
	s32 h1 = ctx->history0, h2 = ctx->history1;
	for (u32 f = 0; f < frames; f++, src += DSPM_FRAME_BYTES) {
		const int ps = src[0];
		const s32 scale = 1 << (ps & 15);
		const s32 c1 = (s16)coefs[((ps >> 4) & 7) * 2];
		const s32 c2 = (s16)coefs[((ps >> 4) & 7) * 2 + 1];
		for (int i = 0; i < DSPM_FRAME_SAMPLES; i++) {
			s32 n = (src[1 + i / 2] >> ((i & 1) ? 0 : 4)) & 15;
			if (n >= 8) n -= 16;
			s64 v = (((s64)(n * scale) << 11) + 1024 + (s64)c1 * h1 + (s64)c2 * h2) >> 11;
			if (v > 32767) v = 32767;
			else if (v < -32768) v = -32768;
			h2 = h1;
			h1 = (s32)v;
			*out = (s16)v;
			out += step;
		}
	}
}

// Reads block `b` and decodes it into buffer slot `i`, ready to queue. Returns
// false on a short read. Touches only `s`, so a start being built off to one
// side needs no lock for it (musicPrepare).
static bool musicFill(MusicStream* s, int i, u32 b)
{
	u32 ch = s->hdr.channels;
	u32 perChan = s->hdr.framesPerBlock * DSPM_FRAME_BYTES;

	if (fseek(s->fp, (long)(s->hdr.dataOffset + (u64)b * s->blockBytes), SEEK_SET) != 0)
		return false;
	if (fread(s->raw, 1, s->blockBytes, s->fp) != s->blockBytes)
		return false;

	// The encoder pads the final block, so clamp it to the real frame count or
	// the DSP would play the padding as a burst of noise.
	u32 first = b * s->hdr.framesPerBlock;
	u32 frames = s->totalFrames - first;
	if (frames > s->hdr.framesPerBlock) frames = s->hdr.framesPerBlock;

	// Interleaved for a stereo stream, so L and R stay sample-locked on one
	// channel.
	for (u32 c = 0; c < ch; c++)
		musicDecode(s->raw + c * perChan, frames, s->coefs[c],
		            &s->index[(size_t)b * ch + c], s->pcm[i] + c, (int)ch);
	u32 n = frames * DSPM_FRAME_SAMPLES;
	DSP_FlushDataCache(s->pcm[i], n * ch * sizeof(s16));

	ndspWaveBuf* w = &s->wb[i];
	memset(w, 0, sizeof(*w));
	w->data_pcm16 = s->pcm[i];
	w->nsamples   = n;
	w->looping    = false;
	w->status     = NDSP_WBUF_FREE;
	return true;
}

// musicFill, then queued on the stream's channel. Caller must hold the lock.
static bool musicSubmit(MusicStream* s, int i, u32 b)
{
	if (!musicFill(s, i, b))
		return false;
	ndspChnWaveBufAdd(s->chan, &s->wb[i]);
	return true;
}

// One streaming tick for a single slot. Caller must hold the lock.
static void musicPump(MusicStream* s, float dt)
{
	if (!s->active) return;

	if (s->fadeRate > 0.0f) {
		float d = s->fadeTarget - s->gain;
		float step = s->fadeRate * dt;
		if (fabsf(d) <= step) { s->gain = s->fadeTarget; s->fadeRate = 0.0f; }
		else                  { s->gain += (d > 0.0f ? step : -step); }
		musicApplyMix(s);
	}

	// How far ahead the DSP is, for the readout: a playing stream with more to
	// come and nothing queued has run dry.
	if (!s->paused && (s->loop || s->nextBlock < s->hdr.blockCount)) {
		int queued = 0;
		for (int i = 0; i < MUSIC_BUFFERS; i++) {
			u8 st = s->wb[i].status;
			queued += st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING;
		}
		if (queued < s_dbgMusicLow) s_dbgMusicLow = queued;
	}

	// At most one block goes in per tick. Reading them all back to back would
	// hold the lock through ~32 KB of SD I/O in one go; one block per tick still
	// refills the whole ring far faster than the DSP drains it.
	for (int i = 0; i < MUSIC_BUFFERS; i++) {
		u8 st = s->wb[i].status;
		if (st != NDSP_WBUF_FREE && st != NDSP_WBUF_DONE)
			continue;
		if (st == NDSP_WBUF_DONE) {
			s->blocksPlayed++;
			s->wb[i].status = NDSP_WBUF_FREE;
		}

		if (s->nextBlock >= s->hdr.blockCount) {
			if (!s->loop) continue;
			s->nextBlock = s->loopBlock;
			s->blocksPlayed = 0;
		}
		if (!musicSubmit(s, i, s->nextBlock))
			{ s->nextBlock = s->hdr.blockCount; continue; }
		s->nextBlock++;
		break;
	}

	if (!s->loop && s->nextBlock >= s->hdr.blockCount) {
		bool busy = false;
		for (int i = 0; i < MUSIC_BUFFERS && !busy; i++) {
			u8 st = s->wb[i].status;
			if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) busy = true;
		}
		if (!busy) { s->drained = true; musicTeardown(s); }
	}
}

// The duck, advanced once per tick and not once per stream. Call with
// s_musicLock held.
static void duckPump(float dt)
{
	const MusicStream* voice = &s_music[AUDIO_MUSIC_SPEECH];
	bool speaking = voice->active && !voice->paused;
	float want = speaking ? clamp01(s_duckLevel) : 1.0f;

	if (want != s_duckTarget) {
		s_duckTarget = want;
		float time = speaking ? DUCK_ATTACK : DUCK_RELEASE;
		s_duckRate = fabsf(s_duckTarget - s_duck) / time;
	}
	if (s_duckRate <= 0.0f)
		return;

	float d = s_duckTarget - s_duck;
	float step = s_duckRate * dt;
	if (fabsf(d) <= step) { s_duck = s_duckTarget; s_duckRate = 0.0f; }
	else                  { s_duck += (d > 0.0f ? step : -step); }

	// Only the music moves; the voice is on its own bus and is not ducked.
	for (int i = 0; i < AUDIO_MUSIC_SLOTS; i++)
		if (s_music[i].active && busIsMusic(s_music[i].bus))
			musicApplyMix(&s_music[i]);
}

// ---------------------------------------------------------------- thread

#define STREAM_TICK_NS 20000000LL  // 20 ms; a 512-frame block lasts ~224 ms

// ---------------------------------------------------------------- packet voices
//
// SND's packet player (SNDPKTPLAY) on NDSP. A voice keeps PACKETS_IN_FLIGHT
// waveBufs queued and refills each one that has played, oldest first, through
// its owner's fill. Retail keeps two packets in flight (GinsuSynthesis's two
// buffers, 0x800B1094); the port keeps three, so a refill that comes a frame
// late still finds one queued. That is the port's choice, not retail's.
//
// The refill runs on its own thread, which the DSP's frame callback (every 160
// output samples, ~4.9 ms) wakes (#310). It used to run in the callback, and
// libctru calls that before it hands the DSP the frame's channel updates: the
// fill takes the synth's lock and the packet lock, both of which the game
// thread holds every frame, and each packet's cache flush is a request to the
// DSP service. A callback held up there is a frame the DSP gets late.
// NFSHP2-3DS, clean on the user's 3DS, does no work in that callback at all.

#define CH_PACKET_FIRST   6
#define PACKETS_IN_FLIGHT 3

typedef struct {
	bool            active;
	int             chan;
	int             capacity;       // samples per packet buffer
	s16*            buf;            // linear memory, PACKETS_IN_FLIGHT buffers
	int             bufCap;         // the capacity buf was allocated for
	ndspWaveBuf     wb[PACKETS_IN_FLIGHT];
	int             next;           // the oldest queued waveBuf
	AudioPacketFill fill;
	void*           user;
	AudioBus        bus;
	float           gain, panL, panR;
	float           rateSent;       // what NDSP was last given (#310)
	float           lowpassSent;    // 0 = filter off, -1 = not given yet
} PacketVoice;

static PacketVoice   s_pkt[AUDIO_PACKET_VOICES];
static LightLock     s_pktLock;
static LightEvent    s_pktWake;
static Thread        s_pktThread;
static volatile bool s_pktRun;

// Debug (#310), since audioDebugDsp(): the DSP thread's frame callback (its
// longest run and the longest gap between two), how long the packet thread
// took to answer a wake, and the fewest packets a voice had left queued when
// it was refilled (0 = it ran dry).
static u64          s_dbgCbLast;
static volatile u32 s_dbgWokeAt, s_dbgCbMax, s_dbgCbGap, s_dbgWakeMax;
static int          s_dbgPktLow = PACKETS_IN_FLIGHT, s_dbgPktDry;
static u32          s_dbgFrames, s_dbgDropped;

// Caller holds s_pktLock.
static void packetApplyMix(PacketVoice* v)
{
	const float g = clamp01(v->gain) * busGain(v->bus);
	mixSet(v->chan, g * clamp01(v->panL), g * clamp01(v->panR));
}

// Every played packet refilled, on the packet thread.
static void packetRefill(void)
{
	LightLock_Lock(&s_pktLock);
	for (int i = 0; i < AUDIO_PACKET_VOICES; i++) {
		PacketVoice* v = &s_pkt[i];
		if (!v->active) continue;
		int played = 0;
		for (int k = 0; k < PACKETS_IN_FLIGHT; k++) played += v->wb[k].status == NDSP_WBUF_DONE;
		if (played == 0) continue;
		if (PACKETS_IN_FLIGHT - played < s_dbgPktLow) s_dbgPktLow = PACKETS_IN_FLIGHT - played;
		if (played == PACKETS_IN_FLIGHT) s_dbgPktDry++;
		for (int k = 0; k < PACKETS_IN_FLIGHT; k++) {
			ndspWaveBuf* w = &v->wb[v->next];
			if (w->status != NDSP_WBUF_DONE) break;
			s16* out = v->buf + v->next * v->capacity;
			int n = v->fill(v->user, out);
			// Port guard: a packet NDSP can take (retail submits what it gets).
			if (n < 1) n = 1;
			if (n > v->capacity) n = v->capacity;
			w->nsamples = (u32)n;
			DSP_FlushDataCache(out, (u32)n * sizeof(s16));
			ndspChnWaveBufAdd(v->chan, w);
			v->next = (v->next + 1) % PACKETS_IN_FLIGHT;
		}
	}
	LightLock_Unlock(&s_pktLock);
}

static void packetThread(void* arg)
{
	(void)arg;
	while (s_pktRun) {
		LightEvent_Wait(&s_pktWake);
		const u32 wait = (u32)svcGetSystemTick() - s_dbgWokeAt;
		if (wait > s_dbgWakeMax) s_dbgWakeMax = wait;
		if (s_pktRun) packetRefill();
	}
}

// The DSP thread's frame callback: wakes the packet thread and runs a frame of
// the output stage, neither of which waits on another thread.
static void frameCallback(void* data)
{
	(void)data;
	const u64 t0 = svcGetSystemTick();
	if (s_dbgCbLast && (u32)(t0 - s_dbgCbLast) > s_dbgCbGap) s_dbgCbGap = (u32)(t0 - s_dbgCbLast);
	s_dbgCbLast = t0;
	if (s_pktThread) {
		s_dbgWokeAt = (u32)t0;
		LightEvent_Signal(&s_pktWake);
	} else {
		packetRefill();   // no thread: the old way, in the callback
	}
	mixPump();
	const u32 d = (u32)(svcGetSystemTick() - t0);
	if (d > s_dbgCbMax) s_dbgCbMax = d;
}

static void packetInit(void)
{
	LightLock_Init(&s_pktLock);
	LightEvent_Init(&s_pktWake, RESET_ONESHOT);
	memset(s_pkt, 0, sizeof(s_pkt));
	for (int i = 0; i < AUDIO_PACKET_VOICES; i++) {
		s_pkt[i].chan = CH_PACKET_FIRST + i;
		s_pkt[i].bus = AUDIO_BUS_ENGINE;
	}
	// On the game's core, above the game and the streaming thread (an 11 ms
	// packet is more urgent than a 224 ms music block), under the DSP thread
	// (0x18).
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	prio -= 2;
	if (prio < 0x19) prio = 0x19;
	s_pktRun = true;
	s_pktThread = threadCreate(packetThread, NULL, 8 * 1024, prio, -2, false);
	if (!s_pktThread) s_pktRun = false;
	s_dbgFrames = ndspGetFrameCount();
	s_dbgDropped = ndspGetDroppedFrames();
	ndspSetCallback(frameCallback, NULL);
}

static void packetExit(void)
{
	ndspSetCallback(NULL, NULL);
	if (s_pktThread) {
		s_pktRun = false;
		LightEvent_Signal(&s_pktWake);
		threadJoin(s_pktThread, U64_MAX);
		threadFree(s_pktThread);
		s_pktThread = NULL;
	}
	LightLock_Lock(&s_pktLock);
	for (int i = 0; i < AUDIO_PACKET_VOICES; i++) {
		PacketVoice* v = &s_pkt[i];
		if (v->active) {
			ndspChnWaveBufClear(v->chan);
			ndspChnReset(v->chan);
			mixCut(v->chan);
			v->active = false;
		}
		if (v->buf) linearFree(v->buf);
		v->buf = NULL;
		v->bufCap = 0;
	}
	LightLock_Unlock(&s_pktLock);
}

static void packetBusChanged(void)
{
	LightLock_Lock(&s_pktLock);
	for (int i = 0; i < AUDIO_PACKET_VOICES; i++)
		if (s_pkt[i].active) packetApplyMix(&s_pkt[i]);
	LightLock_Unlock(&s_pktLock);
}

#define START_MS(a, b) ((double)((b) - (a)) * 1000.0 / (double)SYSCLOCK_ARM11)

// A start, built in `p` off to one side: the file opened, its header and
// index read, the buffers made and every one of them primed with a decoded
// block. No lock and no channel -- all of it is the card and the ARM11, and
// none of it touches a stream that is playing. On failure `p` holds nothing.
static bool musicPrepare(MusicStream* p, const char* path, char* why, size_t n)
{
	const u64 t0 = svcGetSystemTick();
	p->fp = fopen(path, "rb");
	if (!p->fp) {
		snprintf(why, n, "no file (open %.1f ms)", START_MS(t0, svcGetSystemTick()));
		return false;
	}
	const u64 tOpen = svcGetSystemTick();

	DspmHeader* h = &p->hdr;
	if (fread(h, sizeof(*h), 1, p->fp) != 1 || memcmp(h->magic, "DSPM", 4) != 0 ||
	    h->version != 1 || h->channels < 1 || h->channels > 2 ||
	    h->framesPerBlock == 0 || h->blockCount == 0) {
		snprintf(why, n, "not a DSPM v1 file");
		musicRelease(p);
		return false;
	}
	const size_t coefBytes = sizeof(u16) * 16 * h->channels;
	const size_t idxCount  = (size_t)h->blockCount * h->channels;
	p->index = malloc(idxCount * sizeof(ndspAdpcmData));
	if (!p->index || fread(p->coefs, 1, coefBytes, p->fp) != coefBytes ||
	    fread(p->index, sizeof(ndspAdpcmData), idxCount, p->fp) != idxCount) {
		snprintf(why, n, "%s", p->index ? "short header" : "out of memory");
		musicRelease(p);
		return false;
	}
	const u64 tRead = svcGetSystemTick();

	p->blockBytes = h->channels * h->framesPerBlock * DSPM_FRAME_BYTES;
	p->totalFrames = (h->totalSamples + DSPM_FRAME_SAMPLES - 1) / DSPM_FRAME_SAMPLES;
	p->loopBlock = h->loopStart ? h->loopStart / (h->framesPerBlock * DSPM_FRAME_SAMPLES) : 0;
	if (p->loopBlock >= h->blockCount) p->loopBlock = 0;
	p->raw = malloc(p->blockBytes);
	bool mem = p->raw != NULL;
	for (int i = 0; i < MUSIC_BUFFERS && mem; i++) {
		p->pcm[i] = linearAlloc((size_t)h->framesPerBlock * DSPM_FRAME_SAMPLES * h->channels * sizeof(s16));
		mem = p->pcm[i] != NULL;
	}
	if (!mem) {
		snprintf(why, n, "out of memory");
		musicRelease(p);
		return false;
	}

	// Every buffer, before the DSP starts asking for data.
	const u64 tPrime = svcGetSystemTick();
	p->nextBlock = 0;
	while (p->nextBlock < MUSIC_BUFFERS && p->nextBlock < h->blockCount &&
	       musicFill(p, (int)p->nextBlock, p->nextBlock))
		p->nextBlock++;
	if (p->nextBlock == 0) {
		snprintf(why, n, "short read on its first block");
		musicRelease(p);
		return false;
	}
	snprintf(why, n, "open %.1f, header+index %.1f, alloc %.1f, prime %.1f ms",
		START_MS(t0, tOpen), START_MS(tOpen, tRead), START_MS(tRead, tPrime),
		START_MS(tPrime, svcGetSystemTick()));
	return true;
}

// Hands a prepared start to its slot and starts the channel. `s` keeps what
// the ask set -- bus, loop, gain -- and whatever the caller changed since: a
// fade, a gain, a pause. Caller must hold the lock; `p`'s resources are the
// slot's afterwards.
static void musicInstall(MusicStream* s, MusicStream* p, int slot)
{
	s->fp = p->fp;
	s->hdr = p->hdr;
	s->index = p->index;
	memcpy(s->coefs, p->coefs, sizeof(s->coefs));
	s->raw = p->raw;
	for (int i = 0; i < MUSIC_BUFFERS; i++) s->pcm[i] = p->pcm[i];
	s->blockBytes = p->blockBytes;
	s->totalFrames = p->totalFrames;
	s->loopBlock = p->loopBlock;
	s->nextBlock = p->nextBlock;
	s->blocksPlayed = 0;
	s->drained = false;
	s->chan = CH_MUSIC_BASE + slot * 2;

	// Linear, as NFSHP2-3DS's EA Trax: it cannot overshoot full scale, and the
	// polyphase filter does on these masters (#310, musicDecode()).
	ndspChnReset(s->chan);
	ndspChnSetInterp(s->chan, NDSP_INTERP_LINEAR);
	ndspChnSetRate(s->chan, (float)s->hdr.sampleRate);
	ndspChnSetFormat(s->chan, s->hdr.channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	ndspChnSetPaused(s->chan, s->paused);
	musicApplyMix(s);
	// The waveBufs move to the slot before the DSP is given them: it keeps
	// the address it is handed.
	for (int i = 0; i < MUSIC_BUFFERS; i++) {
		if ((u32)i < p->nextBlock) {
			s->wb[i] = p->wb[i];
			ndspChnWaveBufAdd(s->chan, &s->wb[i]);
		} else {
			memset(&s->wb[i], 0, sizeof(s->wb[i]));
			s->wb[i].status = NDSP_WBUF_FREE;
		}
	}
	s->active = true;
}

// The starts audioMusicPlay queued (#342). The frame that asks never waits on
// the card: a speech line's open, in a folder of ~870 files, and its first
// four blocks cost 50-90 ms on Azahar, and on the main thread that was a
// hitch at every Outrun start. Here it all runs outside s_musicLock -- the
// card, the decode -- and only the hand-over takes it, on core 2 (audioInit)
// where none of it is the game's time.
static void musicStartPending(void)
{
	for (int slot = 0; slot < AUDIO_MUSIC_SLOTS; slot++) {
		MusicStream* s = &s_music[slot];
		// An ask that a newer one replaces while it is being opened is thrown
		// away and the newer one taken at once, a few times at most.
		for (int tries = 0; tries < 3; tries++) {
			char path[MUSIC_PATH_MAX];
			LightLock_Lock(&s_musicLock);
			const bool want = s->pending;
			const u32 gen = s->reqGen;
			const u64 asked = s->reqTick;
			if (want) memcpy(path, s->reqPath, sizeof(path));
			LightLock_Unlock(&s_musicLock);
			if (!want) break;

			MusicStream p;
			memset(&p, 0, sizeof(p));
			char why[160];
			const bool ok = musicPrepare(&p, path, why, sizeof(why));

			LightLock_Lock(&s_musicLock);
			const bool current = s->pending && s->reqGen == gen;
			if (current) {
				if (ok) musicInstall(s, &p, slot);
				else    { s->failed = true; s->drained = true; }
				snprintf(s->why, sizeof(s->why), "%s; playing %.1f ms after the ask", why,
					START_MS(asked, svcGetSystemTick()));
				__atomic_store_n(&s->pending, false, __ATOMIC_RELEASE);
			}
			LightLock_Unlock(&s_musicLock);
			if (!(current && ok)) musicRelease(&p);
			if (current) break;
		}
	}
}

static void streamThread(void* arg)
{
	(void)arg;
	u64 last = svcGetSystemTick();
	while (s_threadRun) {
		musicStartPending();
		// The time since the last pump, not the tick: a start can wake this
		// early, or hold it past one.
		const u64 now = svcGetSystemTick();
		float dt = (float)((double)(now - last) / (double)SYSCLOCK_ARM11);
		last = now;
		if (dt > 0.1f) dt = 0.1f;
		LightLock_Lock(&s_musicLock);
		for (int i = 0; i < AUDIO_MUSIC_SLOTS; i++)
			musicPump(&s_music[i], dt);
		// After the pump, because that is where a drained line clears its own
		// `active` -- the music comes back up on the tick the voice ends.
		duckPump(dt);
		LightLock_Unlock(&s_musicLock);
		LightEvent_WaitTimeout(&s_streamWake, STREAM_TICK_NS);
	}
}

// ---------------------------------------------------------------- lifecycle

bool audioInit(void)
{
	if (s_ready) return true;

	// libctru's ndspInit() loads a DSP component from sdmc:/3ds/dspfirm.cdc and
	// does not reliably fail when it is missing -- it faults deep inside
	// ndspSetCounter -- so check for the file before touching ndsp at all.
	if (access("sdmc:/3ds/dspfirm.cdc", F_OK) != 0)
		return false;
	if (R_FAILED(ndspInit()))
		return false;

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetClippingMode(NDSP_CLIP_SOFT);
	// Headroom on the summed mix, before the DSP clips it: music alone can
	// reach full scale, and speech and the effects land on top of it. The
	// port's own value (retail mixes for a TV's amplifier, not two small
	// speakers); NDSP_CLIP_SOFT rounds off whatever still gets over.
	ndspSetMasterVol(AUDIO_HEADROOM);

	LightLock_Init(&s_musicLock);
	LightLock_Init(&s_sfxLock);
	LightLock_Init(&s_mixLock);
	memset(s_mix, 0, sizeof(s_mix));
	s_master = s_masterSent = s_dbgMasterLow = AUDIO_HEADROOM;
	packetInit();   // sets the frame callback, so the output stage is ready first
	memset(s_music, 0, sizeof(s_music));
	memset(s_sfx, 0, sizeof(s_sfx));
	for (int i = 0; i < AUDIO_BUS_COUNT; i++) s_bus[i] = 1.0f;
	for (int i = 0; i < SFX_VOICES; i++) s_sfx[i].chan = s_sfx[i].fadeChan = -1;
	memset(s_chanVoice, -1, sizeof(s_chanVoice));

	s_threadRun = true;
	LightEvent_Init(&s_streamWake, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	prio -= 1;                       // lower number = higher priority on the 3DS
	if (prio < 0x18) prio = 0x18;
	// On the New 3DS's third core (#342, the user's suggestion), so its card
	// waits and its block decodes never take time from the game's core. Core 2
	// is shared with the world loader (world.c, a step below the game) and the
	// boot movie's decoder. This thread is above both, as it was above the game,
	// and idle almost all the time. Each decode and its cache flush run on the
	// same thread, so the PCM reaches the DSP from this core.
	s_thread = threadCreate(streamThread, NULL, 16 * 1024, prio, 2, false);
	if (s_thread) {
		dbglog_line(NULL, "audio: streaming thread on core 2");
	} else {
		s_thread = threadCreate(streamThread, NULL, 16 * 1024, prio, -1, false);
		dbglog_line(NULL, "audio: core 2 refused the streaming thread -- it shares the game's core");
	}
	if (!s_thread) { s_threadRun = false; ndspExit(); return false; }

	s_ready = true;
	return true;
}

void audioExit(void)
{
	if (!s_ready) return;
	s_threadRun = false;
	LightEvent_Signal(&s_streamWake);
	threadJoin(s_thread, U64_MAX);
	threadFree(s_thread);

	LightLock_Lock(&s_musicLock);
	for (int i = 0; i < AUDIO_MUSIC_SLOTS; i++) {
		musicTeardown(&s_music[i]);
		s_music[i].pending = false;
	}
	LightLock_Unlock(&s_musicLock);
	audioSfxStopAll();
	for (int b = 0; b < AUDIO_FEBANK_COUNT; b++)
		audioSfxBankFree((AudioFeBank)b);
	packetExit();

	ndspExit();
	s_ready = false;
}

bool audioAvailable(void) { return s_ready; }

static void cutsceneApplyMix(void);   // the cutscenes section, below

void audioSetBusVolume(AudioBus bus, float gain)
{
	if (!s_ready || bus < 0 || bus >= AUDIO_BUS_COUNT) return;
	LightLock_Lock(&s_musicLock);
	s_bus[bus] = clamp01(gain);
	for (int i = 0; i < AUDIO_MUSIC_SLOTS; i++)
		if (s_music[i].active) musicApplyMix(&s_music[i]);
	LightLock_Unlock(&s_musicLock);
	// AEMS voices hold on for seconds (nitrous, engines), so a volume change
	// reaches them now rather than at their next sound.
	LightLock_Lock(&s_sfxLock);
	for (int i = 0; i < SFX_VOICES; i++)
		if (s_sfx[i].active && s_sfx[i].aems) aemsApplyMix(&s_sfx[i]);
	LightLock_Unlock(&s_sfxLock);
	packetBusChanged();
	if (bus == AUDIO_BUS_SPEECH || bus == AUDIO_BUS_MASTER) cutsceneApplyMix();
}

void audioSetMusicMapGain(float gain)
{
	if (!s_ready) return;
	if (gain < 0.0f) gain = -1.0f;
	if (gain == s_musicMap) return;
	// The render thread calls this every frame, and the streaming thread holds
	// the lock across its SD reads: a busy lock waits for the next frame.
	if (LightLock_TryLock(&s_musicLock) != 0) return;
	s_musicMap = gain;
	for (int i = 0; i < AUDIO_MUSIC_SLOTS; i++)
		if (s_music[i].active && s_music[i].bus == AUDIO_BUS_IG_MUSIC) musicApplyMix(&s_music[i]);
	LightLock_Unlock(&s_musicLock);
}

float audioGetBusVolume(AudioBus bus)
{
	if (bus < 0 || bus >= AUDIO_BUS_COUNT) return 0.0f;
	return s_bus[bus];
}

void audioSetMono(bool mono)
{
	if (!s_ready) return;
	ndspSetOutputMode(mono ? NDSP_OUTPUT_MONO : NDSP_OUTPUT_STEREO);
}

// ---------------------------------------------------------------- music API

const char* audioMusicStartBreakdown(AudioMusicSlot slot)
{
	if (slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return "";
	return s_music[slot].why;
}

bool audioMusicPlay(AudioMusicSlot slot, const char* path, AudioBus bus, bool loop)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS || !path) return false;
	const size_t len = strlen(path);
	if (len == 0 || len >= MUSIC_PATH_MAX) return false;

	// The ask, and only the ask: the streaming thread opens the file and primes
	// the buffers (#342, musicStartPending). What was in the slot stops now, as
	// it always has, and the settings below are the new stream's from here, so
	// a gain, a fade or a pause given before it is playing still holds.
	LightLock_Lock(&s_musicLock);
	MusicStream* s = &s_music[slot];
	musicTeardown(s);
	memcpy(s->reqPath, path, len + 1);
	s->reqGen++;
	s->reqTick = svcGetSystemTick();
	s->bus = bus;
	s->loop = loop;
	s->gain = 1.0f;
	s->fadeRate = 0.0f;
	s->paused = false;
	s->drained = false;
	s->failed = false;
	s->why[0] = '\0';
	__atomic_store_n(&s->pending, true, __ATOMIC_RELEASE);
	LightLock_Unlock(&s_musicLock);
	LightEvent_Signal(&s_streamWake);
	return true;
}

bool audioMusicStarting(AudioMusicSlot slot)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return false;
	return __atomic_load_n(&s_music[slot].pending, __ATOMIC_ACQUIRE);
}

bool audioMusicFailed(AudioMusicSlot slot)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return false;
	if (__atomic_load_n(&s_music[slot].pending, __ATOMIC_ACQUIRE)) return false;
	return s_music[slot].failed;
}

void audioMusicStop(AudioMusicSlot slot)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return;
	LightLock_Lock(&s_musicLock);
	MusicStream* s = &s_music[slot];
	musicTeardown(s);
	// A start still being opened is called off: the streaming thread sees the
	// generation move and throws its work away.
	s->reqGen++;
	if (s->pending) {
		s->drained = true;
		__atomic_store_n(&s->pending, false, __ATOMIC_RELEASE);
	}
	LightLock_Unlock(&s_musicLock);
}

void audioMusicPause(AudioMusicSlot slot, bool paused)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return;
	LightLock_Lock(&s_musicLock);
	MusicStream* s = &s_music[slot];
	if (s->active && s->paused != paused) {
		// The queued blocks stay queued: the pump only refills a block the DSP
		// has finished, and a held channel finishes none.
		ndspChnSetPaused(s->chan, paused);
		s->paused = paused;
	} else if (s->pending) {
		s->paused = paused;   // musicInstall starts the channel held
	}
	LightLock_Unlock(&s_musicLock);
}

bool audioMusicIsPaused(AudioMusicSlot slot)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return false;
	return audioMusicIsPlaying(slot) && s_music[slot].paused;
}

// A start still being opened counts as playing: to its caller it is.
// `pending` is read first, and is cleared only after `active` is set.
bool audioMusicIsPlaying(AudioMusicSlot slot)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return false;
	if (__atomic_load_n(&s_music[slot].pending, __ATOMIC_ACQUIRE)) return true;
	return s_music[slot].active;
}

void audioMusicSetGain(AudioMusicSlot slot, float gain)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return;
	LightLock_Lock(&s_musicLock);
	s_music[slot].gain = clamp01(gain);
	s_music[slot].fadeRate = 0.0f;
	if (s_music[slot].active) musicApplyMix(&s_music[slot]);
	LightLock_Unlock(&s_musicLock);
}

float audioMusicGetGain(AudioMusicSlot slot)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return 0.0f;
	return s_music[slot].gain;
}

void audioMusicFadeTo(AudioMusicSlot slot, float target, float seconds)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return;
	LightLock_Lock(&s_musicLock);
	MusicStream* s = &s_music[slot];
	s->fadeTarget = clamp01(target);
	if (seconds <= 0.0f) {
		s->gain = s->fadeTarget;
		s->fadeRate = 0.0f;
		if (s->active) musicApplyMix(s);
	} else {
		s->fadeRate = fabsf(s->fadeTarget - s->gain) / seconds;
		if (s->fadeRate <= 0.0f) s->fadeRate = 0.0f;
	}
	LightLock_Unlock(&s_musicLock);
}

void audioSetMusicDuckLevel(float gain)
{
	LightLock_Lock(&s_musicLock);
	s_duckLevel = clamp01(gain);
	LightLock_Unlock(&s_musicLock);
}

float audioMusicDuck(void)
{
	return s_duck;
}

float audioMusicPosition(AudioMusicSlot slot)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return 0.0f;
	MusicStream* s = &s_music[slot];
	if (!s->active || !s->hdr.sampleRate) return 0.0f;
	u64 samples = (u64)s->blocksPlayed * s->hdr.framesPerBlock * DSPM_FRAME_SAMPLES;
	return (float)((double)samples / s->hdr.sampleRate);
}

float audioMusicDuration(AudioMusicSlot slot)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return 0.0f;
	MusicStream* s = &s_music[slot];
	if (!s->active || !s->hdr.sampleRate) return 0.0f;
	return (float)((double)s->hdr.totalSamples / s->hdr.sampleRate);
}

bool audioMusicFinished(AudioMusicSlot slot)
{
	if (!s_ready || slot < 0 || slot >= AUDIO_MUSIC_SLOTS) return false;
	if (__atomic_load_n(&s_music[slot].pending, __ATOMIC_ACQUIRE)) return false;
	return s_music[slot].drained && !s_music[slot].active;
}

// ---------------------------------------------------------------- cutscenes

// The movie voice's mix: the front pair at the speech bus's gain (#99).
static void cutsceneApplyMix(void)
{
	float mix[12];
	memset(mix, 0, sizeof(mix));
	mix[0] = mix[1] = busGain(AUDIO_BUS_SPEECH);
	ndspChnSetMix(AUDIO_CH_CUTSCENE, mix);
}

void audioCutsceneStart(uint32_t rate, int channels)
{
	if (!s_ready) return;
	ndspChnReset(AUDIO_CH_CUTSCENE);
	ndspChnSetInterp(AUDIO_CH_CUTSCENE, NDSP_INTERP_LINEAR);
	ndspChnSetRate(AUDIO_CH_CUTSCENE, (float)rate);
	ndspChnSetFormat(AUDIO_CH_CUTSCENE, channels == 2
		? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	cutsceneApplyMix();
	char msg[96];
	snprintf(msg, sizeof(msg), "audio: movie voice %lu Hz, %d ch, at %.2f (speech x master)",
		(unsigned long)rate, channels, busGain(AUDIO_BUS_SPEECH));
	dbglog_line(NULL, msg);
}

void audioCutsceneQueue(ndspWaveBuf* wb)
{
	if (!s_ready || !wb) return;
	ndspChnWaveBufAdd(AUDIO_CH_CUTSCENE, wb);
}

bool audioCutscenePlaying(void)
{
	return s_ready && ndspChnIsPlaying(AUDIO_CH_CUTSCENE);
}

void audioCutsceneStop(void)
{
	if (!s_ready) return;
	// Order matters: drop the queue first so the DSP stops reading the buffer,
	// then reset the channel. The caller frees the PCM only after this returns.
	ndspChnWaveBufClear(AUDIO_CH_CUTSCENE);
	ndspChnReset(AUDIO_CH_CUTSCENE);
}

// ---------------------------------------------------------------- sfx

// Minimal RIFF/WAVE PCM16 reader. SFX are short, so the whole clip goes into
// linear memory as a single waveBuf and the DSP plays it without further help.
static s16* loadWav(const char* path, u32* outSamples, u32* outRate, u32* outCh)
{
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;

	char riff[12];
	if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) {
		fclose(f); return NULL;
	}

	u32 rate = 0, ch = 0, bits = 0;
	s16* pcm = NULL; u32 samples = 0;
	for (;;) {
		char id[4]; u32 sz;
		if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) break;
		if (!memcmp(id, "fmt ", 4)) {
			u16 fmt, chans, blockAlign, bps;
			u32 sr, byteRate;
			fread(&fmt, 2, 1, f); fread(&chans, 2, 1, f); fread(&sr, 4, 1, f);
			fread(&byteRate, 4, 1, f); fread(&blockAlign, 2, 1, f); fread(&bps, 2, 1, f);
			rate = sr; ch = chans; bits = bps;
			if (sz > 16) fseek(f, (long)sz - 16, SEEK_CUR);
		} else if (!memcmp(id, "data", 4)) {
			if (bits != 16 || ch < 1 || ch > 2) break;
			pcm = linearAlloc(sz);
			if (!pcm) break;
			if (fread(pcm, 1, sz, f) != sz) { linearFree(pcm); pcm = NULL; break; }
			samples = sz / (2 * ch);
			break;
		} else {
			fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR);
		}
	}
	fclose(f);
	if (!pcm) return NULL;
	*outSamples = samples; *outRate = rate; *outCh = ch;
	return pcm;
}

int audioSfxPlay(const char* path, AudioBus bus, float gain, float pan)
{
	if (!s_ready) return -1;

	u32 samples, rate, ch;
	s16* pcm = loadWav(path, &samples, &rate, &ch);
	if (!pcm) return -1;

	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = sfxVoiceTake(1.0f);
	if (!v) { LightLock_Unlock(&s_sfxLock); linearFree(pcm); return -1; }

	v->active = true;
	v->aems = false;
	v->pcm = pcm;

	float g = clamp01(gain) * busGain(bus);
	float mix[12];
	buildMix(mix, g, pan);

	ndspChnReset(v->chan);
	ndspChnSetInterp(v->chan, NDSP_INTERP_LINEAR);
	ndspChnSetRate(v->chan, (float)rate);
	ndspChnSetFormat(v->chan, ch == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	mixCut(v->chan);
	mixSet(v->chan, mix[0], mix[1]);

	DSP_FlushDataCache(pcm, samples * ch * sizeof(s16));
	memset(&v->wb, 0, sizeof(v->wb));
	v->wb.data_pcm16 = pcm;
	v->wb.nsamples   = samples;
	ndspChnWaveBufAdd(v->chan, &v->wb);

	int id = v->chan;
	LightLock_Unlock(&s_sfxLock);
	return id;
}

void audioSfxStopAll(void)
{
	if (!s_ready) return;
	LightLock_Lock(&s_sfxLock);
	for (int i = 0; i < SFX_VOICES; i++) {
		if (!s_sfx[i].active) continue;
		s_sfx[i].aems = false;   // so sfxChanDrop ends a loop too
		sfxChanDrop(&s_sfx[i]);
		if (s_sfx[i].pcm) { linearFree(s_sfx[i].pcm); s_sfx[i].pcm = NULL; }
		s_sfx[i].active = false;
	}
	mixCutFades();
	LightLock_Unlock(&s_sfxLock);
}

void audioSfxBankFree(AudioFeBank bank)
{
	if (!s_ready || bank < 0 || bank >= AUDIO_FEBANK_COUNT) return;
	LightLock_Lock(&s_sfxLock);
	// A voice playing one of this bank's clips is still reading that linear
	// block, so it has to be stopped before the block goes. Voices on their
	// own PCM (audioSfxPlay) are left alone -- they own what they are
	// reading -- and so are AEMS voices, which read their own bank's block.
	for (int i = 0; i < SFX_VOICES; i++) {
		if (!s_sfx[i].active || s_sfx[i].pcm || s_sfx[i].aems || s_sfx[i].bank != (int)bank)
			continue;
		sfxChanDrop(&s_sfx[i]);
	}
	SfxClip* clips = s_clip[bank];
	for (int i = 0; i < SFX_BANK_MAX; i++)
		if (clips[i].data) linearFree(clips[i].data);
	memset(clips, 0, sizeof(s_clip[bank]));
	s_clipCount[bank] = 0;
	LightLock_Unlock(&s_sfxLock);
}

bool audioSfxBankLoad(AudioFeBank bank, const char* path)
{
	if (!s_ready || bank < 0 || bank >= AUDIO_FEBANK_COUNT) return false;
	audioSfxBankFree(bank);

	FILE* f = fopen(path, "rb");
	if (!f) return false;

	SfxBankHeader bh;
	u32 table[SFX_BANK_MAX * 2];
	bool ok = fread(&bh, 1, sizeof(bh), f) == sizeof(bh) &&
	          memcmp(bh.magic, "FSFX", 4) == 0 && bh.version == 1 &&
	          bh.count > 0 && bh.count <= SFX_BANK_MAX;
	if (ok)
		ok = fread(table, 8, bh.count, f) == bh.count;
	if (!ok) { fclose(f); return false; }

	int n = 0;
	for (u32 i = 0; i < bh.count; i++) {
		const u32 base = table[i * 2];
		DspmHeader h;
		if (fseek(f, (long)base, SEEK_SET) != 0) break;
		if (fread(&h, 1, sizeof(h), f) != sizeof(h)) break;
		// One mono block per clip is what the builder writes. Anything else
		// would need the music path's block index and its streaming thread.
		if (memcmp(h.magic, "DSPM", 4) != 0 || h.channels != 1 || h.blockCount != 1)
			break;

		SfxClip* c = &s_clip[bank][n];
		u16 idx; s16 hist0, hist1;
		if (fread(c->coefs, 2, 16, f) != 16) break;
		if (fread(&idx, 2, 1, f) != 1) break;
		if (fread(&hist0, 2, 1, f) != 1) break;
		if (fread(&hist1, 2, 1, f) != 1) break;
		c->start.index    = idx;
		c->start.history0 = hist0;
		c->start.history1 = hist1;
		c->bytes   = h.framesPerBlock * DSPM_FRAME_BYTES;
		c->samples = h.totalSamples;
		c->rate    = h.sampleRate;
		c->data    = linearAlloc(c->bytes);
		if (!c->data) break;
		if (fseek(f, (long)(base + h.dataOffset), SEEK_SET) != 0 ||
				fread(c->data, 1, c->bytes, f) != c->bytes) {
			linearFree(c->data);
			c->data = NULL;
			break;
		}
		// dspenc's index is 0 (#310): the first frame header is the real one.
		c->start.index = c->data[0];
		DSP_FlushDataCache(c->data, c->bytes);
		n++;
	}
	fclose(f);
	s_clipCount[bank] = n;
	return n > 0;
}

int audioSfxBankCount(AudioFeBank bank)
{
	return bank >= 0 && bank < AUDIO_FEBANK_COUNT ? s_clipCount[bank] : 0;
}

int audioSfxBankPlay(AudioFeBank bank, int index, AudioBus bus, float gain, float pan)
{
	if (!s_ready || bank < 0 || bank >= AUDIO_FEBANK_COUNT || index < 0 ||
			index >= s_clipCount[bank])
		return -1;
	SfxClip* c = &s_clip[bank][index];
	if (!c->data) return -1;

	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = sfxVoiceTake(1.0f);
	if (!v) { LightLock_Unlock(&s_sfxLock); return -1; }

	v->active = true;
	v->aems = false;
	v->bank = (int)bank;
	v->pcm = NULL;   // the bank owns the block; the voice only reads it

	float g = clamp01(gain) * busGain(bus);
	float mix[12];
	buildMix(mix, g, pan);

	ndspChnReset(v->chan);
	ndspChnSetInterp(v->chan, NDSP_INTERP_LINEAR);   // #310, as musicDecode()
	ndspChnSetRate(v->chan, (float)c->rate);
	ndspChnSetFormat(v->chan, NDSP_FORMAT_ADPCM);
	ndspChnSetAdpcmCoefs(v->chan, c->coefs);
	mixCut(v->chan);
	mixSet(v->chan, mix[0], mix[1]);

	// Per voice, not per clip: the same click can be in flight twice.
	v->adp = c->start;
	memset(&v->wb, 0, sizeof(v->wb));
	v->wb.data_adpcm = c->data;
	v->wb.nsamples   = c->samples;
	v->wb.adpcm_data = &v->adp;
	ndspChnWaveBufAdd(v->chan, &v->wb);

	int id = v->chan;
	LightLock_Unlock(&s_sfxLock);
	return id;
}

// ---------------------------------------------------------------- AEMS voices
//
// The backend aems.c's player nodes drive (aems.h). A handle is
// (gen << 8) | voice index, so a node still holding the handle of a voice
// that ended and was reused gets "not alive" rather than someone else's sound.

#define AEMS_RATE_MAX (4.0f * 32728.0f)   // 4x the DSP's output rate

static int s_aemsGen;

// Caller holds s_sfxLock.
static SfxVoice* aemsVoiceGet(int h)
{
	if (h < 0) return NULL;
	int i = h & 0xff;
	if (i >= SFX_VOICES) return NULL;
	SfxVoice* v = &s_sfx[i];
	return (v->active && v->aems && v->gen == (h >> 8)) ? v : NULL;
}

static void aemsApplyMix(SfxVoice* v)
{
	float g = clamp01(v->gain) * busGain(v->bus);
	mixSet(v->chan, g * clamp01(v->panL), g * clamp01(v->panR));
}

static void aemsApplyRate(SfxVoice* v)
{
	if (v->chan < 0) return;
	float rate = (float)v->clip->rate * v->pitch;
	if (rate < 1.0f) rate = 1.0f;
	if (rate > AEMS_RATE_MAX) rate = AEMS_RATE_MAX;
	ndspChnSetRate(v->chan, rate);
}

// The voice's clip from the top on the channel it has just been given.
// Caller holds s_sfxLock.
static void aemsChanStart(SfxVoice* v)
{
	const AemsClip* clip = v->clip;
	ndspChnReset(v->chan);
	ndspChnSetInterp(v->chan, NDSP_INTERP_LINEAR);   // #310, as musicDecode()
	aemsApplyRate(v);
	ndspChnSetFormat(v->chan, NDSP_FORMAT_ADPCM);
	ndspChnSetAdpcmCoefs(v->chan, (u16*)clip->coefs);
	mixCut(v->chan);
	aemsApplyMix(v);

	v->adp.index    = clip->ps;
	v->adp.history0 = clip->hist0;
	v->adp.history1 = clip->hist1;
	memset(&v->wb, 0, sizeof(v->wb));
	v->wb.data_adpcm = (u8*)clip->data;
	v->wb.nsamples   = clip->samples;
	v->wb.adpcm_data = &v->adp;
	v->wb.looping    = clip->loop;
	ndspChnWaveBufAdd(v->chan, &v->wb);
	if (v->paused) ndspChnSetPaused(v->chan, true);
}

int aemsVoicePlay(const AemsClip* clip, int bus, float pitch, float gain,
                  float panLeft, float panRight)
{
	if (!s_ready || !clip || !clip->data || !clip->samples) return -1;
	if (bus < 0 || bus >= AUDIO_BUS_COUNT) bus = AUDIO_BUS_SFX;

	LightLock_Lock(&s_sfxLock);
	const int i = sfxVoiceFree();
	if (i < 0) { s_dbgRefused++; LightLock_Unlock(&s_sfxLock); return -1; }
	SfxVoice* v = &s_sfx[i];

	v->aems   = true;
	v->orphan = 0;
	v->pcm    = NULL;   // the AEMS bank owns the block
	v->clip   = clip;
	v->bus    = (AudioBus)bus;
	v->pitch  = pitch;
	v->gain   = gain;
	v->panL   = panLeft;
	v->panR   = panRight;
	v->paused = false;
	v->chan   = -1;

	// No channel for it: a loop starts virtual, a one-shot is refused.
	const int ch = sfxChanFind(sfxLevel(v));
	if (ch < 0 && !clip->loop) {
		v->aems = false;
		s_dbgRefused++;
		LightLock_Unlock(&s_sfxLock);
		return -1;
	}
	s_aemsGen = (s_aemsGen + 1) & 0x7fffff;
	v->active = true;
	v->gen    = s_aemsGen;
	if (ch >= 0) {
		sfxChanGive(i, ch);
		aemsChanStart(v);
	}

	int h = (v->gen << 8) | i;
	LightLock_Unlock(&s_sfxLock);
	return h;
}

bool aemsVoiceAlive(int voice)
{
	if (!s_ready) return false;
	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = aemsVoiceGet(voice);
	bool alive = v && (v->chan < 0 || v->wb.status != NDSP_WBUF_DONE);
	LightLock_Unlock(&s_sfxLock);
	return alive;
}

const AemsClip* aemsVoiceClip(int voice)
{
	if (!s_ready) return NULL;
	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = aemsVoiceGet(voice);
	const AemsClip* c = v ? v->clip : NULL;
	LightLock_Unlock(&s_sfxLock);
	return c;
}

void aemsVoiceSetPitch(int voice, float pitch)
{
	if (!s_ready) return;
	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = aemsVoiceGet(voice);
	if (v) { v->pitch = pitch; aemsApplyRate(v); }
	LightLock_Unlock(&s_sfxLock);
}

void aemsVoiceSetGain(int voice, float gain)
{
	if (!s_ready) return;
	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = aemsVoiceGet(voice);
	if (v) { v->gain = gain; aemsApplyMix(v); }
	LightLock_Unlock(&s_sfxLock);
}

void aemsVoiceSetPan(int voice, float panLeft, float panRight)
{
	if (!s_ready) return;
	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = aemsVoiceGet(voice);
	if (v) { v->panL = panLeft; v->panR = panRight; aemsApplyMix(v); }
	LightLock_Unlock(&s_sfxLock);
}

void aemsVoicePause(int voice, bool paused)
{
	if (!s_ready) return;
	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = aemsVoiceGet(voice);
	if (v) {
		v->paused = paused;
		if (v->chan >= 0) ndspChnSetPaused(v->chan, paused);
	}
	LightLock_Unlock(&s_sfxLock);
}

void aemsVoiceStop(int voice)
{
	if (!s_ready) return;
	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = aemsVoiceGet(voice);
	if (v) {
		// Faded, not cut (#310): the channel stays out of the pool until
		// mixPump() ends the fade and clears it.
		if (v->chan >= 0) {
			mixFadeStop(v->chan);
			v->fadeChan = (s8)v->chan;
			s_chanVoice[v->chan] = -1;
			v->chan = -1;
		}
		v->active = false;
		v->aems = false;
		v->orphan = 0;
	}
	LightLock_Unlock(&s_sfxLock);
}

uint32_t aemsVoiceSamplePos(int voice)
{
	if (!s_ready) return 0;
	LightLock_Lock(&s_sfxLock);
	SfxVoice* v = aemsVoiceGet(voice);
	uint32_t pos = v && v->chan >= 0 ? ndspChnGetSamplePos(v->chan) : 0;
	LightLock_Unlock(&s_sfxLock);
	return pos;
}

int aemsVoicesPlaying(void)
{
	if (!s_ready) return 0;
	int n = 0;
	LightLock_Lock(&s_sfxLock);
	for (int i = 0; i < SFX_VOICES; i++)
		n += s_sfx[i].active && s_sfx[i].aems && s_sfx[i].chan >= 0 &&
		     s_sfx[i].wb.status != NDSP_WBUF_DONE && ndspChnIsPlaying(s_sfx[i].chan);
	LightLock_Unlock(&s_sfxLock);
	return n;
}

// #351: a traffic horn was heard on for minutes, through the pause menu, its
// channel at one level all the while -- nothing steering it, nothing to stop
// it. Whatever left it, it is ended here once it has been so for half a
// second, and the log says what it was.
#define REAP_FRAMES 30

static u8 s_chanOrphan[MIX_CHANNELS];

int aemsVoiceReap(const int32_t* held, int n)
{
	if (!s_ready) return 0;
	int cut = 0;
	char msg[160];
	LightLock_Lock(&s_sfxLock);
	for (int i = 0; i < SFX_VOICES; i++) {
		SfxVoice* v = &s_sfx[i];
		if (!v->active || !v->aems) continue;
		const int32_t h = (v->gen << 8) | i;
		bool mine = false;
		for (int k = 0; k < n && !mine; k++) mine = held[k] == h;
		if (mine) { v->orphan = 0; continue; }
		if (++v->orphan < REAP_FRAMES) continue;
		snprintf(msg, sizeof(msg),
		         "audio: orphan AEMS voice %d cut -- %lu Hz %lu smp %s, ch %d, pitch %.2f gain %.3f pan %.2f/%.2f",
		         i, (unsigned long)(v->clip ? v->clip->rate : 0),
		         (unsigned long)(v->clip ? v->clip->samples : 0),
		         v->clip && v->clip->loop ? "loop" : "once", v->chan, v->pitch, v->gain, v->panL, v->panR);
		dbglog_line(NULL, msg);
		if (v->chan >= 0) {
			mixFadeStop(v->chan);
			v->fadeChan = (s8)v->chan;
			s_chanVoice[v->chan] = -1;
			v->chan = -1;
		}
		v->active = false;
		v->aems = false;
		v->orphan = 0;
		cut++;
	}
	for (int k = 0; k < SFX_CHANNELS; k++) {
		const int ch = s_sfxChan[k];
		if (s_chanVoice[ch] >= 0 || mixFading(ch) || !ndspChnIsPlaying(ch)) {
			s_chanOrphan[ch] = 0;
			continue;
		}
		if (++s_chanOrphan[ch] < REAP_FRAMES) continue;
		s_chanOrphan[ch] = 0;
		float mix[12];
		ndspChnGetMix(ch, mix);
		snprintf(msg, sizeof(msg),
		         "audio: orphan channel %d cut -- no voice, mix state %d, rate %.2f mix %.3f/%.3f pos %lu",
		         ch, s_mix[ch].state, ndspChnGetRate(ch), mix[0], mix[1],
		         (unsigned long)ndspChnGetSamplePos(ch));
		dbglog_line(NULL, msg);
		ndspChnWaveBufClear(ch);
		ndspChnReset(ch);
		mixCut(ch);
		cut++;
	}
	LightLock_Unlock(&s_sfxLock);
	return cut;
}

int audioDebugChannel(int ch, char* out, size_t n)
{
	if (!s_ready || ch < 0 || ch > CH_SFX_LAST || !out || n == 0) return 0;
	if (!ndspChnIsPlaying(ch)) return 0;
	float mix[12];
	ndspChnGetMix(ch, mix);
	return snprintf(out, n, "ch %d %s rate %.2f mix %.3f/%.3f pos %lu%s",
	                ch, sfxChanIs(ch) ? "sfx" : ch >= CH_PACKET_FIRST ? "pkt" : "music",
	                ndspChnGetRate(ch), mix[0], mix[1], (unsigned long)ndspChnGetSamplePos(ch),
	                ndspChnIsPaused(ch) ? " paused" : "");
}

int audioDebugMix(char* out, size_t n)
{
	if (!s_ready || !out || n == 0) return 0;
	LightLock_Lock(&s_sfxLock);
	int virt = 0;
	for (int i = 0; i < SFX_VOICES; i++) virt += s_sfx[i].active && s_sfx[i].chan < 0;
	LightLock_Lock(&s_mixLock);
	int fading = 0;
	for (int ch = 0; ch < MIX_CHANNELS; ch++) fading += s_mix[ch].state == MIX_FADING;
	int len = snprintf(out, n, "output: load peak %.2f, master low %.2f (now %.2f), %d fading, %d start(s) refused, "
	                   "%d channel(s) taken, %d virtual now, music queue low %d/%d",
	                   s_dbgLoad, s_dbgMasterLow, s_master, fading, s_dbgRefused, s_dbgStolen, virt,
	                   s_dbgMusicLow, MUSIC_BUFFERS);
	s_dbgLoad = 0.0f;
	s_dbgMasterLow = s_master;
	s_dbgRefused = s_dbgStolen = 0;
	s_dbgMusicLow = MUSIC_BUFFERS;
	LightLock_Unlock(&s_mixLock);
	LightLock_Unlock(&s_sfxLock);
	return len;
}

int audioDebugDsp(char* out, size_t n)
{
	if (!s_ready || !out || n == 0) return 0;
	const u32 frames = ndspGetFrameCount(), dropped = ndspGetDroppedFrames();
	int len = snprintf(out, n, "dsp: %lu frames, %lu dropped; frame callback max %lu us, longest gap %lu us; "
	                   "packet wake max %lu us; engine packets low %d/%d, ran dry %d",
	                   (unsigned long)(frames - s_dbgFrames), (unsigned long)(dropped - s_dbgDropped),
	                   (unsigned long)(s_dbgCbMax / CPU_TICKS_PER_USEC),
	                   (unsigned long)(s_dbgCbGap / CPU_TICKS_PER_USEC),
	                   (unsigned long)(s_dbgWakeMax / CPU_TICKS_PER_USEC),
	                   s_dbgPktLow, PACKETS_IN_FLIGHT, s_dbgPktDry);
	s_dbgFrames = frames;
	s_dbgDropped = dropped;
	s_dbgCbMax = s_dbgCbGap = s_dbgWakeMax = 0;
	s_dbgPktLow = PACKETS_IN_FLIGHT;
	s_dbgPktDry = 0;
	return len;
}

void aemsVoiceCutFades(void)
{
	if (!s_ready) return;
	mixCutFades();
}

// PORT (#330): a bank's players stop their voices as its modules go, but a
// voice record keeps its clip pointer for as long as it is active, and a loop
// that is virtual when its bank is freed is given a channel back whenever it
// is loud enough -- aemsChanStart then reads the freed clip table. A tester's
// 11-02 hardware crash was exactly that: a data abort in
// ndspChnSetAdpcmCoefs from audioUpdate, coefs 0x02960296, three Quick Races
// in. So whatever lost the voice, it never outlives the samples.
int aemsVoiceDropClips(const AemsClip* clips, uint32_t count)
{
	if (!s_ready || !clips || !count) return 0;
	int cut = 0;
	LightLock_Lock(&s_sfxLock);
	for (int i = 0; i < SFX_VOICES; i++) {
		SfxVoice* v = &s_sfx[i];
		if (!v->active || !v->aems || v->clip < clips || v->clip >= clips + count) continue;
		v->aems = false;   // so sfxChanDrop ends a loop too
		sfxChanDrop(v);
		v->active = false;
		cut++;
	}
	LightLock_Unlock(&s_sfxLock);
	return cut;
}

void audioUpdate(void)
{
	if (!s_ready) return;
	LightLock_Lock(&s_sfxLock);
	for (int i = 0; i < SFX_VOICES; i++) {
		SfxVoice* v = &s_sfx[i];
		if (!v->active || v->chan < 0 || v->wb.status != NDSP_WBUF_DONE) continue;
		ndspChnReset(v->chan);
		mixCut(v->chan);
		s_chanVoice[v->chan] = -1;
		v->chan = -1;
		if (v->pcm) { linearFree(v->pcm); v->pcm = NULL; }
		v->active = false;
	}
	// The virtual loops heard again get channels back, the loudest first; once
	// one finds none, a quieter one would not either.
	for (;;) {
		int best = -1;
		float loudest = SFX_SILENT;
		for (int i = 0; i < SFX_VOICES; i++) {
			const SfxVoice* v = &s_sfx[i];
			if (!v->active || v->chan >= 0) continue;
			const float g = sfxLevel(v);
			if (g > loudest) { loudest = g; best = i; }
		}
		if (best < 0) break;
		const int ch = sfxChanFind(loudest);
		if (ch < 0) break;
		sfxChanGive(best, ch);
		aemsChanStart(&s_sfx[best]);
	}
	LightLock_Unlock(&s_sfxLock);
}

// ---------------------------------------------------------------- packet voices (API)

bool audioPacketStart(int voice, uint32_t rate, int capacity, int silentLen,
                      AudioPacketFill fill, void* user)
{
	if (!s_ready || voice < 0 || voice >= AUDIO_PACKET_VOICES || !fill || rate == 0 ||
	    capacity < 1 || silentLen < 1 || silentLen > capacity)
		return false;
	LightLock_Lock(&s_pktLock);
	PacketVoice* v = &s_pkt[voice];
	if (v->active) {
		ndspChnWaveBufClear(v->chan);
		v->active = false;
	}
	mixCut(v->chan);
	if (!v->buf || v->bufCap < capacity) {
		if (v->buf) linearFree(v->buf);
		v->buf = linearAlloc((size_t)PACKETS_IN_FLIGHT * (size_t)capacity * sizeof(s16));
		v->bufCap = v->buf ? capacity : 0;
	}
	if (!v->buf) {
		LightLock_Unlock(&s_pktLock);
		return false;
	}
	v->capacity = capacity;
	v->fill = fill;
	v->user = user;
	v->next = 0;
	// The first packets are silence, as StartSynthesis submits them.
	memset(v->buf, 0, (size_t)PACKETS_IN_FLIGHT * (size_t)capacity * sizeof(s16));
	DSP_FlushDataCache(v->buf, (u32)(PACKETS_IN_FLIGHT * capacity * sizeof(s16)));
	ndspChnReset(v->chan);
	// #310, as musicDecode(): Ginsu's packets peak at full scale too.
	ndspChnSetInterp(v->chan, NDSP_INTERP_LINEAR);
	ndspChnSetRate(v->chan, (float)rate);
	ndspChnSetFormat(v->chan, NDSP_FORMAT_MONO_PCM16);
	v->rateSent = (float)rate;
	v->lowpassSent = -1.0f;
	packetApplyMix(v);
	for (int k = 0; k < PACKETS_IN_FLIGHT; k++) {
		memset(&v->wb[k], 0, sizeof(v->wb[k]));
		v->wb[k].data_pcm16 = v->buf + k * capacity;
		v->wb[k].nsamples = (u32)silentLen;
		ndspChnWaveBufAdd(v->chan, &v->wb[k]);
	}
	v->active = true;
	LightLock_Unlock(&s_pktLock);
	return true;
}

void audioPacketStop(int voice)
{
	if (!s_ready || voice < 0 || voice >= AUDIO_PACKET_VOICES) return;
	LightLock_Lock(&s_pktLock);
	PacketVoice* v = &s_pkt[voice];
	if (v->active) {
		ndspChnWaveBufClear(v->chan);
		ndspChnReset(v->chan);
		mixCut(v->chan);
		v->active = false;
	}
	LightLock_Unlock(&s_pktLock);
}

void audioPacketSetRate(int voice, float rate)
{
	if (!s_ready || voice < 0 || voice >= AUDIO_PACKET_VOICES) return;
	if (rate < 1.0f) rate = 1.0f;
	if (rate > AEMS_RATE_MAX) rate = AEMS_RATE_MAX;
	LightLock_Lock(&s_pktLock);
	PacketVoice* v = &s_pkt[voice];
	// #310: the engine sets it every frame; NDSP hears only of a change.
	if (v->active && rate != v->rateSent) {
		ndspChnSetRate(v->chan, rate);
		v->rateSent = rate;
	}
	LightLock_Unlock(&s_pktLock);
}

void audioPacketSetMix(int voice, int bus, float gain, float panLeft, float panRight)
{
	if (!s_ready || voice < 0 || voice >= AUDIO_PACKET_VOICES) return;
	if (bus < 0 || bus >= AUDIO_BUS_COUNT) bus = AUDIO_BUS_ENGINE;
	LightLock_Lock(&s_pktLock);
	PacketVoice* v = &s_pkt[voice];
	v->bus = (AudioBus)bus;
	v->gain = gain;
	v->panL = panLeft;
	v->panR = panRight;
	if (v->active) packetApplyMix(v);
	LightLock_Unlock(&s_pktLock);
}

void audioPacketSetLowpass(int voice, float hz)
{
	if (!s_ready || voice < 0 || voice >= AUDIO_PACKET_VOICES) return;
	LightLock_Lock(&s_pktLock);
	PacketVoice* v = &s_pkt[voice];
	// NDSP mixes at 32728 Hz: a cutoff at or past its Nyquist is open.
	const float want = hz > 0.0f && hz < 16000.0f ? hz : 0.0f;
	// #310: the engine sets it every frame, and libctru marks the filter dirty
	// on every call, changed or not; NDSP hears only of a change.
	if (v->active && want != v->lowpassSent) {
		if (want == 0.0f) {
			ndspChnIirMonoSetEnable(v->chan, false);
		} else {
			// A one-pole low-pass, y = (1 - p) x + p y[-1]. libctru's
			// ndspChnIirMonoSetParamsLowPassFilter has the two swapped (b0 = p,
			// a1 = p - 1): a filter that cuts the bass and keeps the top, and
			// rings at Nyquist as the cutoff nears 16 kHz. The custom filter
			// sets these and switches the filter on.
			const float p = expf(-6.2831853f * want / (float)NDSP_SAMPLE_RATE);
			ndspChnIirMonoSetParamsCustomFilter(v->chan, 1.0f, -p, 1.0f - p);
		}
		v->lowpassSent = want;
	}
	LightLock_Unlock(&s_pktLock);
}

void audioPacketPause(int voice, bool paused)
{
	if (!s_ready || voice < 0 || voice >= AUDIO_PACKET_VOICES) return;
	LightLock_Lock(&s_pktLock);
	if (s_pkt[voice].active) ndspChnSetPaused(s_pkt[voice].chan, paused);
	LightLock_Unlock(&s_pktLock);
}
