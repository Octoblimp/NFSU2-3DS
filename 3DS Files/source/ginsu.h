// Ginsu: EA's granular engine synth, the player car's engine loop.
//
// A .gin file (SOUND/ENGINE/GIN_*.gin) is one recorded engine sweep, idle to
// redline, in 4-bit XA-style ADPCM, with a table of where each engine cycle
// starts and one of which sample sounds at which RPM. GinsuSynthesis plays it
// in 11 ms packets: each packet carries on from where the last one stopped and,
// to follow the RPM, jumps whole cycles forwards or back with a 0.5 ms
// crossfade -- so the pitch and timbre are the recording's own at every RPM.
//
// Retail: Speed.elf GinsuSynthData (0x800AFC28..0x800B07E8) and GinsuSynthesis
// (0x800B0908..0x800B1624), source ginsu_ps.c; the PC's twin reads the same
// little-endian files. The packets go out through audio.c's packet voices
// (audioPacket*), the 3DS side of SND's SNDPKTPLAY.
#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {               // GinsuSynthData
	void*          file;       // the port's: the loaded .gin (owned)
	float          minFreq;    // +0x04 the RPM of the first sample
	float          maxFreq;    // +0x08 ... of the last
	int32_t        nf;         // +0x0C frequency table steps
	int32_t        nc;         // +0x10 cycles
	int32_t        ns;         // +0x14 samples
	int32_t        rate;       // +0x18 Hz
	const int32_t* freq;       // +0x1C nf + 1 sample positions, minFreq .. maxFreq
	const int32_t* cyc;        // +0x20 nc + 1 cycle starts
	const uint8_t* data;       // +0x24 19-byte blocks of 32 samples
	float          minCycle;   // +0x28 the shortest cycle, in samples
	float          block[32];  // +0x2C the decoded block
	int32_t        blockIdx;   // +0xAC
} GinsuData;

// Loads and binds a .gin (BindToData 0x800AFD84). False if it is missing or
// not a version-2 Ginsu file.
bool ginsuLoad(GinsuData* g, const char* path);
void ginsuFree(GinsuData* g);

typedef struct {               // GinsuSynthesis
	int        voice;          // the port's: audio.c packet voice (+0x1C's SNDPKTPLAY)
	int32_t    capacity;       // +0x0C samples per packet buffer
	int32_t    jumpReset;      // +0x10 samples between cycle jumps
	int32_t    packetLen;      // +0x14
	int32_t    xfade;          // +0x18
	int32_t    playing;        // +0x20 the voice's handle, -1 when stopped
	int32_t    rate;           // +0x24 the data's rate while playing, else 0
	GinsuData* data;           // +0x28
	int32_t    pos;            // +0x2C the next sample to play
	int32_t    s30;            // +0x30 the sample the current RPM sounds at
	int32_t    s34;            // +0x34 ... the target RPM does
	int32_t    glide;          // +0x38 packets left to reach it
	int32_t    s3c;            // +0x3C samples since the last jump, counting down
	float      cyc;            // +0x40 the cycle at s30
	int32_t    peak;           // debug: the loudest sample since ginsuTakePeak
	LightLock  lock;           // SNDSYS_entercritical: the DSP callback reads the rest
} GinsuSynth;

// GinsuSynthesis::GinsuSynthesis (0x800B1094) on packet voice `voice` (0, 1).
void  ginsuSynthInit(GinsuSynth* s, int voice);
// SetSynthData (0x800B11A0). False if the packets would not fit.
bool  ginsuSetSynthData(GinsuSynth* s, GinsuData* d);
// StartSynthesis (0x800B1358) at an RPM: >= 0 once the voice plays.
int   ginsuStart(GinsuSynth* s, float freq);
// UpdateFrequency (0x800B14CC): glide to `freq` over `ms`.
void  ginsuUpdateFrequency(GinsuSynth* s, float freq, float ms);
// GetCurrentPitch (0x800B15A0): engine cycles per second now playing.
float ginsuCurrentPitch(GinsuSynth* s);
// StopSynthesis (0x800B1624).
void  ginsuStop(GinsuSynth* s);
// Debug: the loudest sample played since the last call, 0..32768.
int32_t ginsuTakePeak(GinsuSynth* s);
