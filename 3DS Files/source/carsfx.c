// The cars' CARSFX controllers over the AEMS runtime. See carsfx.h.
#include "carsfx.h"
#include "cardb.h"

#include "aems.h"
#include "audio.h"
#include "colsfx.h"
#include "dbglog.h"
#include "difficulty.h"
#include "emevents.h"
#include "envsfx.h"
#include "ginsu.h"
#include "mixmap.h"
#include "rideinfo.h"
#include "sfxline.h"
#include "speech.h"
#include "trackpath.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void sfxLog(const char* fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	dbglog_line("CARSFX ", b);
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------- mix map
//
// Retail's SFX objects read their volume and azimuth off the mix map's output
// words (mixmap.h) through SFX_Base::GetQ15Output (0x800BE8B0) and
// GetAzimOutput (0x800BE8D8), by output. Without the map loaded, these give
// full volume, straight ahead.
//
// The opponents' turbo, nitrous and skids are the PlayerCar state's own
// classes (CSTATEMGR_Base::CreateSFX, 0x800A913C, falls back to them for the
// AICar state), each instance reading its own copy's object in the map. They
// play through the player's banks: every CARSFX's ctor (0x80097CE4) sets its
// +0x54, so CSTATE_Base::LoadData (0x800ABF74) skips SetupLoadData for a
// class SFX_Base::IsSFXBankLoaded already has from the PlayerCar state.

// ---------------------------------------------------------------- pitch

// EAXSound's pause clock: +0x1C paused, +0x24 when it began, +0x28 now.
static bool  s_paused;
static float s_pausedFor;

// SFX_Base::AdjustPitch: a second into the pause menu every sound's pitch goes
// to 0, which is how retail holds them. (Its other case, the window losing
// focus, has no 3DS counterpart.)
static void adjustPitch(int32_t* pitch)
{
	if (s_paused && s_pausedFor > 1.0f)
		*pitch = 0;
}

// ---------------------------------------------------------------- nitrous

typedef struct {
	AemsBank* bank;          // +0xA0, the bank SetupLoadData picked (the player's)
	int       upgrade;       // +0xAC, GetUpgradeLevel(NITROUS)
	int       state;         // +0xB8: 0 waiting, 1 the bottle's sound is on
	AemsInst  nos;           // +0xB0's instance, FX_NITROUS
	AemsInst  purge;         // +0xB4's instance, FX_PURGE
	int32_t   nosData[4];    // level, volume, azimuth, stop
	int32_t   purgeData[4];  // volume, azimuth, stop, pitch
	uint32_t  sfxId;         // its object in the mix map: the player's, or an opponent's copy
	char      who[8];        // debug: "" the player, "AI n " an opponent
} Nitrous;

static Nitrous s_nos;

// Debug: half a second after the player's nitrous changes, the log says what
// is sounding.
static float s_reportIn;

static int32_t nosQ15(const Nitrous* n, int output)  { return mixmapQ15(n->sfxId, output); }
static int32_t nosAzim(const Nitrous* n, int output) { return mixmapAzim(n->sfxId, output); }

// Car::IsBlowOff: the bottle is open but the car is not using it -- the
// throttle shut (|throttle| <= 0.001) or the car under 5 mph (|v| * 2.237).
static bool isBlowOff(const CarSfxIn* in)
{
	if (fabsf(in->throttle - 0.0f) <= 0.001f || in->speed * 2.237f < 5.0f)
		return in->nosLevel != 0;
	return false;
}

// Release, as Play / PlayPurge / ProcessUpdate do it: the game's hold goes,
// and any graph still running winds down on its own.
static void letGo(AemsInst* inst)
{
	if (*inst) aemsRelease(*inst);
	*inst = NULL;
}

// CARSFX_Nitrous::Play(level, -, -). UpdateParams passes level 0.
static void nosPlay(Nitrous* n, int level)
{
	const int32_t vol = nosQ15(n, 0);
	letGo(&n->nos);
	const int32_t azim = nosAzim(n, 0);
	n->nosData[0] = clampi(level, 0, 2);
	n->nosData[1] = clampi(vol, 0, 0x7fff);
	n->nosData[2] = clampi(azim, 0, 0xffff);
	n->nosData[3] = 0;
	n->nos = aemsCreate(aemsClass("FX_NITROUS"), n->nosData);
}

// CARSFX_Nitrous::Stop: the stop flag, which the graph answers with the
// blow-off and a fade.
static void nosStop(Nitrous* n)
{
	if (!n->nos) return;
	n->nosData[3] = 1;
	aemsSetMemberData(n->nos, n->nosData);
}

// CARSFX_Nitrous::PlayPurge: a new FX_PURGE, every field 0 until
// ProcessUpdate fills them in.
static void nosPlayPurge(Nitrous* n)
{
	letGo(&n->purge);
	for (int i = 0; i < 4; i++) n->purgeData[i] = 0;
	n->purge = aemsCreate(aemsClass("FX_PURGE"), n->purgeData);
}

static void nosStopPurge(Nitrous* n)
{
	if (!n->purge) return;
	n->purgeData[2] = 1;
	aemsSetMemberData(n->purge, n->purgeData);
}

// CARSFX_Nitrous::UpdateParams. Not ported from it: the GB_NOS stream a human
// driver's overfilled bottle (GetNosPercentageLeft > 1.5) queues in a race
// through NIS_AudioParser, and the +0xCC fade line (0 -> 1 over 400 ms on,
// back to 0 over 1 s off), which nothing ported reads. An opponent's plays
// through the player's bank, so without that it is silent too.
static void nosUpdateParams(Nitrous* n, const CarSfxIn* in)
{
	if (!s_nos.bank) return;
	const bool on = in->nosLevel != 0;
	const bool blowOff = isBlowOff(in);
	const bool player = n == &s_nos;
	if (n->state == 0) {
		if (on && !blowOff) {
			n->state = 1;
			nosPlay(n, 0);
			if (player) s_reportIn = 0.5f;
			sfxLog("%snitrous: on (level %d, Nitrous_%02d)", n->who, in->nosLevel,
			     (s_nos.upgrade == 2 || s_nos.upgrade == 3) ? 1 : 0);
		}
	} else if (!on || blowOff) {
		if (n->nos) nosStop(n);
		n->state = 0;
		if (player) s_reportIn = 0.5f;
		sfxLog("%snitrous: off%s", n->who, blowOff ? " (blow-off)" : "");
	}
	if (blowOff) {
		if (!n->purge) {
			nosPlayPurge(n);
			if (player) s_reportIn = 0.5f;
			sfxLog("%snitrous: purge", n->who);
		}
	} else if (n->purge && n->purgeData[2] != 1) {
		nosStopPurge(n);
	}
}

// CARSFX_Nitrous::ProcessUpdate: an instance whose graphs have all finished
// (refcount down to the game's own 1) is let go; a live one gets this frame's
// volume and azimuth, and the purge its pitch.
static void nosProcessUpdate(Nitrous* n)
{
	if (n->nos) {
		if (aemsRefCount(n->nos) <= 1) {
			letGo(&n->nos);
			sfxLog("%sFX_NITROUS: its graphs are done, let go (%d voice(s) sounding)", n->who,
			       aemsVoicesPlaying());
		} else {
			const int32_t vol = nosQ15(n, 0);
			int32_t pitch = 0x1000;
			adjustPitch(&pitch);   // and FX_NITROUS has no field for it
			n->nosData[1] = clampi(vol, 0, 0x7fff);
			n->nosData[2] = clampi(nosAzim(n, 0), 0, 0xffff);
			aemsSetMemberData(n->nos, n->nosData);
		}
	}
	if (n->purge) {
		if (aemsRefCount(n->purge) <= 1) {
			letGo(&n->purge);
			sfxLog("%sFX_PURGE: its graphs are done, let go (%d voice(s) sounding)", n->who,
			       aemsVoicesPlaying());
		} else {
			int32_t pitch = 0x1000;
			adjustPitch(&pitch);
			n->purgeData[0] = clampi(nosQ15(n, 1), 0, 0x7fff);
			n->purgeData[1] = clampi(nosAzim(n, 1), 0, 0xffff);
			n->purgeData[3] = clampi(pitch, 0, 0x3fff);
			aemsSetMemberData(n->purge, n->purgeData);
		}
	}
}

// ---------------------------------------------------------------- hydraulics

// CARSFX_Hydraulics (#313), SFX 8 of the PlayerCar state: Hydraulics_00_MB
// (SetupLoadData's pszSndPaths[6], the NOS folder), an FX_Hydraulic per corner
// while it rises (type 1) or lowers (type 2) and one FX_Hydr_Bounce while a
// bounce kicks. Member names other than the type and the volume are not
// decoded; retail fills them with 0 (Play's p2 / p4 / p5), as here. PORT: the
// bank is loaded for every car, where retail's gate on it is not traced.
typedef struct {
	AemsBank* bank;          // +0xA0
	AemsInst  inst[4];       // +0xB8, FX_Hydraulic
	AemsInst  bounce;        // +0xC8, FX_Hydr_Bounce
	int32_t   data[4][6];
	int32_t   bounceData[4];
	int       cur[4];        // +0xCC
	int       last[4];       // +0xDC
	bool      bflag[4];      // +0xF8
} Hydraulics;

static Hydraulics s_hy;

static int32_t hyQ15(void) { return clampi(mixmapQ15(MIXMAP_SFX_HYDRAULICS, 0), 0, 0x7fff); }

// Destroy (0x800A10A0): corner i's sound stopped and let go.
static void hyDestroy(int i)
{
	letGo(&s_hy.inst[i]);
}

// Play (0x800A0EA4): a new FX_Hydraulic of `type` for corner i, which covers
// every corner of the group moving with it.
static void hyPlay(int type, int i)
{
	if (!s_hy.bank) return;
	hyDestroy(i);
	int32_t* d = s_hy.data[i];
	d[0] = 0;
	d[1] = 0;
	d[2] = 0;
	d[3] = clampi(type, 1, 2);
	d[4] = 0;
	d[5] = 0;
	s_hy.inst[i] = aemsCreate(aemsClass("FX_Hydraulic"), d);
	s_hy.last[i] = s_hy.cur[i];
	for (int j = 0; j < 4; j++)
		if (s_hy.cur[j] == 2 || (s_hy.cur[j] == 3 && !s_hy.inst[j]))
			s_hy.last[j] = s_hy.cur[j];
	sfxLog("hydraulics: FX_Hydraulic type %d for corner %d (%s)", type, i,
	       type == 1 ? "rising" : "lowering");
}

// UpdateParams (0x800A1134): a finished corner's sound let go, the states read,
// the bounce's sound made while a corner kicks, then one change of state per
// update played or stopped.
static void hyUpdateParams(const CarSfxIn* in)
{
	if (!s_hy.bank) return;
	bool any = false;
	for (int i = 0; i < 4; i++) {
		if (s_hy.inst[i] && aemsRefCount(s_hy.inst[i]) == 1 && s_hy.cur[i] < 2)
			hyDestroy(i);
		s_hy.cur[i] = in->hydState[i];
		s_hy.bflag[i] = in->hydFired[i];
		any |= s_hy.bflag[i];
	}
	if (s_hy.bounce && aemsRefCount(s_hy.bounce) < 2)
		letGo(&s_hy.bounce);
	if (any && !s_hy.bounce) {
		s_hy.bounceData[0] = 0;
		s_hy.bounceData[1] = 0;
		s_hy.bounceData[2] = 0x6f54;
		s_hy.bounceData[3] = 0;
		s_hy.bounce = aemsCreate(aemsClass("FX_Hydr_Bounce"), s_hy.bounceData);
		sfxLog("hydraulics: FX_Hydr_Bounce");
	}
	for (int i = 0; i < 4; i++) {
		if (s_hy.cur[i] == s_hy.last[i])
			continue;
		if (s_hy.cur[i] == 2)
			hyPlay(1, i);
		else if (s_hy.cur[i] == 3)
			hyPlay(2, i);
		else {
			hyDestroy(i);
			s_hy.last[i] = s_hy.cur[i];
		}
		return;
	}
}

// ProcessUpdate (0x800A1408): the mix map's volume on every live instance.
static void hyProcessUpdate(void)
{
	const int32_t vol = hyQ15();
	for (int i = 0; i < 4; i++) {
		if (!s_hy.inst[i]) continue;
		s_hy.data[i][4] = vol;
		aemsSetMemberData(s_hy.inst[i], s_hy.data[i]);
	}
	if (s_hy.bounce) {
		s_hy.bounceData[2] = vol;
		aemsSetMemberData(s_hy.bounce, s_hy.bounceData);
	}
}

static void hyShutdown(void)
{
	for (int i = 0; i < 4; i++)
		letGo(&s_hy.inst[i]);
	letGo(&s_hy.bounce);
	if (s_hy.bank) aemsUnload(s_hy.bank);
	memset(&s_hy, 0, sizeof(s_hy));
}

// ---------------------------------------------------------------- engine

// CARSFX_PlayerEngine: the AEMS CAR graph (the bank's own engine layers, the
// redline, the induction) and one Ginsu synth -- two for a car with a decel
// recording -- driven by the controllers' revs and levels.
typedef struct {
	const EngineData* eng;   // CSTATE +0x7C
	AemsBank*  bank;         // the CAR_xx_ENG_MB_SPU bank
	AemsInst   car;          // +0xB0's instance
	int32_t    carData[26];  // ... its member data
	GinsuData  gin[2];       // +0xCC / +0xE8: the accel and decel recordings
	GinsuSynth syn[2];
	int        handle[2];    // +0xE0 / +0xFC: StartSynthesis' voice, -1 none
	bool       started;      // +0xC8: InitializeEngine found the synths playing
	float      pitch;        // +0xBC: 1, retail's pitch multiplier
	float      rpm;          // +0x104: the RPM the Ginsu plays, which the CAR graph follows
	int32_t    vol[2];       // +0x108 / +0x10C: the synths' SND volumes, 0..127
	AudioBus   bus;
} PlayerEngine;

static PlayerEngine s_eng;

// The mix map's output for the engine (GetQ15Output / GetAzimOutput 0).
static int32_t engQ15(void)  { return mixmapQ15(MIXMAP_SFX_PLAYERENGINE, 0); }
static int32_t engAzim(void) { return mixmapAzim(MIXMAP_SFX_PLAYERENGINE, 0); }

// The bank's .abk name with the port's .aem in its place.
static void bankPath(char* out, size_t n, const char* audioDir, const char* abk)
{
	char name[64];
	snprintf(name, sizeof(name), "%s", abk);
	char* dot = strrchr(name, '.');
	if (dot) *dot = '\0';
	snprintf(out, n, "%sENGINE/%s.aem", audioDir, name);
}

// SetupLoadData, RegisterData and InitializeEngine: load the CAR bank and the
// Ginsu recordings, start the synths at their lowest RPM, and create the CAR
// instance with every field 0 but the engine ID.
static void engSetup(const char* audioDir, const EngineData* e, const DualGinsuData* d, AudioBus bus)
{
	char path[512];
	s_eng.eng = e;
	s_eng.bus = bus;
	s_eng.pitch = 1.0f;
	s_eng.handle[0] = s_eng.handle[1] = -1;
	bankPath(path, sizeof(path), audioDir, e->spuBank);
	s_eng.bank = aemsLoad(path, bus);
	if (!s_eng.bank)
		sfxLog("engine: no %s -- its AEMS layers silent (tools/abk3ds.py)", path);
	const int n = e->dual ? 2 : 1;
	for (int i = 0; i < n; i++) {
		const char* gin = i == 0 ? e->gin : d->gin;
		snprintf(path, sizeof(path), "%sENGINE/%s", audioDir, gin);
		ginsuSynthInit(&s_eng.syn[i], i);
		if (!ginsuLoad(&s_eng.gin[i], path)) {
			sfxLog("engine: no %s -- that Ginsu silent", path);
			continue;
		}
		if (ginsuSetSynthData(&s_eng.syn[i], &s_eng.gin[i]))
			s_eng.handle[i] = ginsuStart(&s_eng.syn[i], s_eng.gin[i].minFreq);
		sfxLog("engine: %s %s, %d Hz, RPM %.0f..%.0f, voice %d", i == 0 ? "accel" : "decel", gin,
		       (int)s_eng.gin[i].rate, s_eng.gin[i].minFreq, s_eng.gin[i].maxFreq, s_eng.handle[i]);
	}
	s_eng.started = e->dual ? (s_eng.handle[0] >= 0 && s_eng.handle[1] >= 0) : s_eng.handle[0] >= 0;
	memset(s_eng.carData, 0, sizeof(s_eng.carData));
	s_eng.carData[0] = clampi(e->id, 0, 0x3c);
	if (s_eng.bank) s_eng.car = aemsCreate(aemsClass("CAR"), s_eng.carData);
	sfxLog("engine: %s, %s Ginsu%s, CAR instance %s", e->spuBank, e->dual ? "dual" : "single",
	       s_eng.started ? "" : " (not started)", s_eng.car ? "up" : "none");
}

// One synth's SND parameters, as SetGinsuParams*Mix sets them. Not ported:
// SNDCTRL_drylevel and SNDfxlevel, the reverb sends (r13 - 0x45F8, - 0x45D0),
// which the port's mixer has no reverb for.
static void ginsuMix(int i, int32_t level, int32_t q15, float ratio, float lowpass)
{
	s_eng.vol[i] = clampi(level * q15 >> 23, 0, 0x7f);
	float l, r;
	aemsAzimPan(engAzim(), &l, &r);
	audioPacketSetMix(i, s_eng.bus, (float)s_eng.vol[i] / 127.0f, l, r);
	audioPacketSetRate(i, (float)s_eng.gin[i].rate * (float)(int32_t)(ratio * s_eng.pitch * 4096.0f) / 4096.0f);
	audioPacketSetLowpass(i, (float)(int32_t)lowpass);
}

// SetGinsuParamsSingleMix / DualMix: below the recording's lowest RPM the
// synth holds it and the voice's pitch comes down instead; the RPM the CAR
// graph hears is the one the accel synth is playing.
static void engGinsuParams(const EngineCtlMix* m)
{
	if (!s_eng.started) return;
	const int32_t q15 = engQ15();
	float rpm = s_eng.rpm;
	const float minRpm = s_eng.gin[0].minFreq;
	float ratio = 1.0f;
	if (rpm < minRpm) {
		ratio = rpm / minRpm;
		rpm = minRpm;
	}
	ginsuMix(0, m->ginVol, q15, ratio, m->lowpass);
	ginsuUpdateFrequency(&s_eng.syn[0], rpm, 60.0f);
	s_eng.rpm = ginsuCurrentPitch(&s_eng.syn[0]) * ratio * 120.0f;
	if (m->dual) {
		ginsuMix(1, m->decelVol, q15, ratio, m->lowpass);
		ginsuUpdateFrequency(&s_eng.syn[1], rpm, 60.0f);
	}
}

// SetEngineParams (0x800A0950).
static void engSetEngineParams(void)
{
	const EngineCtlMix* m = enginectlMix();
	s_eng.pitch = 1.0f;
	s_eng.rpm = m->ginRpm;
	engGinsuParams(m);
	if (!s_eng.car) return;
	const int32_t q15 = engQ15();
	// Port stand-in: two sound-environment globals (r13 - 0x45C8, - 0x45CC)
	// the port has not identified, taken at rest (0x7FFF and 0): fields 13
	// and 14 read 0.
	const int32_t env0 = 0x7fff, env1 = 0;
	const int32_t aems = m->aemsVol * q15 >> 15;
	int32_t* c = s_eng.carData;
	c[4] = clampi(aems - 0x7fff, -0x7fff, 0);
	c[5] = clampi(aems - 0x7fff, -0x7fff, 0);
	c[25] = clampi(m->redVol * q15 >> 15, 0, 0x7fff);
	c[1] = clampi((int32_t)s_eng.rpm, 0, 10000);
	c[16] = clampi((int32_t)(s_eng.pitch * 16383.0f) - 0x3fff, -0x3fff, 0x3fff);
	c[3] = clampi((int32_t)(m->torque * 1024.0f * 0.01f), 0, 0x400);
	c[17] = clampi(m->f130, 0, 0x400);
	c[15] = clampi(engAzim(), 0, 0x10000);
	c[13] = clampi(env0 - 0x7fff, -0x7fff, 0);
	c[14] = clampi(q15 * env1 >> 15, 0, 0x7fff);
	aemsSetMemberData(s_eng.car, c);
}

static void engShutdown(void)
{
	for (int i = 0; i < 2; i++) {
		ginsuStop(&s_eng.syn[i]);
		ginsuFree(&s_eng.gin[i]);
	}
	letGo(&s_eng.car);
	if (s_eng.bank) aemsUnload(s_eng.bank);
	memset(&s_eng, 0, sizeof(s_eng));
	s_eng.handle[0] = s_eng.handle[1] = -1;
}

// ---------------------------------------------------------------- shift

// CARSFX_Shift (ctor 0x80098344, SetupSFX 0x80098428, UpdateParams
// 0x800987A4, PlayShiftSnd 0x80098E78, PlayDisengageSnd 0x80098A48,
// PlayEngageSnd 0x80098C6C, ProcessUpdate 0x80099194, UpdateMixerOutputs
// 0x80098404, SetupLoadData 0x800AEC40): the gearbox's clunk on every shift
// (FX_SHIFTING_01, the shift pattern's GEAR bank) and, over 7000 revs, the
// engine's own rasp as an up-shift lets the clutch go and takes it up again
// (CAR_SWTN, the engine's SWTN_CAR sweetener bank).
typedef struct {
	const ShiftPattern*  pat;      // +0xD0: the gearbox level's pattern, Shifting's own
	const SweetenerData* swtn;     // +0xD4: kSweetener[CSTATE +0x78]
	AemsBank* gearBank;
	AemsBank* swtnBank;
	AemsInst  shift;               // +0xB0's instance, FX_SHIFTING_01
	AemsInst  disengage;           // +0xBC's, CAR_SWTN
	AemsInst  engage;              // +0xC0's, CAR_SWTN
	int32_t   shiftData[6];        // up, volume, pitch, azimuth, 0, in-car view
	int32_t   disData[7];          // engage, engine, revs, volume, azimuth, 0, 0
	int32_t   engData[7];
	// Port guard: PlayDisengageSnd / PlayEngageSnd store the new instance over
	// one still sounding without letting go of it, so retail keeps that one's
	// hold for good. Here it waits in this list, and is let go once its graphs
	// are done (refcount < 2), as UpdateParams would have: it sounds out the
	// same, without the leak.
	AemsInst  orphan[4];
} Shift;

static Shift s_sh;

static int32_t shQ15(int output)  { return mixmapQ15(MIXMAP_SFX_SHIFT, output); }
static int32_t shAzim(int output) { return mixmapAzim(MIXMAP_SFX_SHIFT, output); }

// The instance still held is set aside, not dropped (see Shift.orphan).
static void shiftOrphan(AemsInst* inst)
{
	if (!*inst) return;
	for (int i = 0; i < 4; i++) {
		if (!s_sh.orphan[i]) {
			s_sh.orphan[i] = *inst;
			*inst = NULL;
			return;
		}
	}
	letGo(inst);   // four already waiting: let this one wind down
}

// ShiftingAttackVolSlope (PC 0x7F095C, Slope::GetValue 0x4599E0): 26000 at
// 4000 revs up to 32767 at 7000. Both callers ask only over 7000.
static float shiftAttackVol(float x)
{
	const float out0 = 26000.0f, out1 = 32767.0f, in0 = 4000.0f, in1 = 7000.0f;
	float f = (x - in0) / (in1 - in0);
	f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
	return out0 + (out1 - out0) * f;
}

// PlayShiftSnd: the clunk, quieter the higher the revs (a quarter cosine from
// 1500 to 8000), from the pattern's up or down level.
static void shiftPlayShift(const EngineCtlMix* m, int pov)
{
	const bool up = (uint32_t)(m->shiftState - 1) < 3;
	const int32_t patVol = up ? SHIFT_I(s_sh.pat, 0x04) : SHIFT_I(s_sh.pat, 0x3C);
	float f = ((float)(int32_t)m->sndRpm - 1500.0f) / 6500.0f;
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	const int32_t c = kCosTable[(int32_t)(512.0f - (f * 0.9f + 0.1f) * 512.0f)];
	const int32_t vol = (patVol * c >> 15) * shQ15(up ? 0 : 1) >> 15;
	letGo(&s_sh.shift);
	s_sh.shiftData[0] = up ? 1 : 0;
	s_sh.shiftData[1] = clampi(vol, 0, 0x7fff);
	s_sh.shiftData[2] = 0x1000;
	s_sh.shiftData[3] = clampi(shAzim(0), 0, 0xffff);
	s_sh.shiftData[4] = 0;
	s_sh.shiftData[5] = pov == 2;
	s_sh.shift = aemsCreate(aemsClass("FX_SHIFTING_01"), s_sh.shiftData);
	sfxLog("shift: %s, FX_SHIFTING_01 volume %d at %d revs (map %d)", up ? "up" : "down",
	       (int)s_sh.shiftData[1], (int)m->sndRpm, (int)shQ15(up ? 0 : 1));
}

// PlayDisengageSnd / PlayEngageSnd: CAR_SWTN field 0 says which.
static void shiftPlaySwtn(const EngineCtlMix* m, bool engage)
{
	const float x = engage ? m->rpmFrom : m->sndRpm;
	if (!(7000.0f <= x)) return;
	const int32_t att = (int32_t)shiftAttackVol(x);
	const int32_t vol = (s_sh.swtn->vol * att >> 15) * shQ15(engage ? 2 : 3) >> 15;
	int32_t* d = engage ? s_sh.engData : s_sh.disData;
	AemsInst* inst = engage ? &s_sh.engage : &s_sh.disengage;
	shiftOrphan(inst);
	d[0] = engage ? 1 : 0;
	d[1] = clampi(s_eng.eng ? s_eng.eng->id : 0, 0, 0x3c);   // CSTATE +0x7C's +8
	d[2] = clampi((int32_t)m->sndRpm, 0, 10000);
	d[3] = clampi(vol, 0, 0x8013);
	d[4] = clampi(shAzim(0), 0, 0x10000);
	d[5] = 0;
	d[6] = 0;
	*inst = aemsCreate(aemsClass("CAR_SWTN"), d);
	sfxLog("shift: CAR_SWTN %s, volume %d at %d revs", engage ? "engage" : "disengage",
	       (int)d[3], (int)x);
}

// UpdateParams: a rasp whose graphs are done is let go, then whatever
// Shifting flagged this frame plays.
static void shiftUpdateParams(const EngineCtlMix* m, int pov)
{
	if (!s_sh.pat) return;
	AemsInst* held[2] = { &s_sh.disengage, &s_sh.engage };
	for (int i = 0; i < 2; i++)
		if (*held[i] && aemsRefCount(*held[i]) < 2) letGo(held[i]);
	for (int i = 0; i < 4; i++)
		if (s_sh.orphan[i] && aemsRefCount(s_sh.orphan[i]) < 2) letGo(&s_sh.orphan[i]);
	if (m->holdOver) shiftPlayShift(m, pov);
	if (m->upBegan) shiftPlaySwtn(m, false);
	if (m->upCaught) shiftPlaySwtn(m, true);
}

// ProcessUpdate: only the pause does anything -- each instance's volume to 0.
static void shiftProcessUpdate(bool paused)
{
	if (!paused) return;
	if (s_sh.shift) {
		s_sh.shiftData[1] = 0;
		aemsSetMemberData(s_sh.shift, s_sh.shiftData);
	}
	if (s_sh.disengage) {
		s_sh.disData[3] = 0;
		aemsSetMemberData(s_sh.disengage, s_sh.disData);
	}
	if (s_sh.engage) {
		s_sh.engData[3] = 0;
		aemsSetMemberData(s_sh.engage, s_sh.engData);
	}
}

// SetupSFX + SetupLoadData: the pattern's GEAR bank (SHIFTING) and, when the
// entry names one, the sweetener's (ENGINE).
static void shiftSetup(const char* audioDir, const ShiftPattern* pat, int engIdx, AudioBus bus)
{
	char path[512], name[64];
	s_sh.pat = pat;
	s_sh.swtn = &kSweetener[engIdx < 0x2c && engIdx >= 0 ? engIdx : 0];
	snprintf(name, sizeof(name), "%s", pat->bank);
	char* dot = strrchr(name, '.');
	if (dot) *dot = '\0';
	snprintf(path, sizeof(path), "%sSHIFTING/%s.aem", audioDir, name);
	s_sh.gearBank = aemsLoad(path, bus);
	if (!s_sh.gearBank)
		sfxLog("shift: no %s -- the gear change silent (tools/abk3ds.py)", path);
	if (s_sh.swtn->bank[0]) {
		bankPath(path, sizeof(path), audioDir, s_sh.swtn->bank);
		s_sh.swtnBank = aemsLoad(path, bus);
		if (!s_sh.swtnBank)
			sfxLog("shift: no %s -- the clutch rasp silent (tools/abk3ds.py)", path);
	}
	sfxLog("shift: %s, sweetener %s (%d)", pat->bank, s_sh.swtn->bank, (int)s_sh.swtn->vol);
}

static void shiftShutdown(void)
{
	letGo(&s_sh.shift);
	letGo(&s_sh.disengage);
	letGo(&s_sh.engage);
	for (int i = 0; i < 4; i++) letGo(&s_sh.orphan[i]);
	if (s_sh.gearBank) aemsUnload(s_sh.gearBank);
	if (s_sh.swtnBank) aemsUnload(s_sh.swtnBank);
	memset(&s_sh, 0, sizeof(s_sh));
}

// ---------------------------------------------------------------- spark chatter

// CARSFX_SparkChatter (ctor 0x8009B2AC, SetupSFX 0x8009B424, InitSFX
// 0x8009B4A4, SubScribeToCallback 0x8009B5EC, SparkChatCreateCallBack
// 0x8009B634, UpdateMixerOutputs 0x8009B6A0, Destroy 0x8009B70C, UpdateParams
// 0x8009B7A0, ProcessUpdate 0x8009B870; SPEED2.EXE's ProcessUpdate 0x45F800):
// the exhaust's backfire pops. One CAR_Sputter instance, from the engine's
// SWTN_CAR bank (Shift's -- its own SetupLoadData 0x800AF068 loads nothing),
// lives as long as the car; each SFXMessage 9 (carsim_backfire) flips its
// trigger field, and its graph plays the sputter. The graph reports back
// through a CAR_SputOutput instance it makes, tagged with ours, whose first
// field goes to the mix map (output +8 on, +0xC the value), where an
// envelope reads it.
#define SC_TAG 0x5350   // retail's tag is the CARSFX's own address

typedef struct {
	bool      enabled;     // +0xA0: InitSFX ran
	const SweetenerData* swtn;   // +0xBC: kSweetener[CSTATE +0x78]
	AemsInst  inst;        // +0xB4's instance, CAR_Sputter
	// +0xB4 + 4: engine, tag, revs, volume, azimuth, 0, 0, torque, trigger,
	// 0, shifting
	int32_t   data[11];
	bool      pending;     // CSTATE +0xE8 (PC +0xF0): EAXTunerCar::SFXMessage 9
	int32_t   output;      // +0xF8: CAR_SputOutput's field 0
	AemsInst  out[4];      // the CAR_SputOutput instances carrying our tag
	int       logs;        // debug
} SparkChatter;

static SparkChatter s_sc;

// SparkChatCreateCallBack, then the member-data client it subscribes
// (SparkChatUpdateCallBack -> ReceiveSparkChatterInputs): an instance whose
// field 2 is our tag is ours, and its field 0 is kept.
static void scOutput(AemsInst inst, const int32_t* p, bool made, void* user)
{
	(void)user;
	int mine = -1;
	for (int i = 0; i < 4; i++)
		if (s_sc.out[i] == inst) mine = i;
	if (made) {
		if (s_sc.logs < 8) {
			s_sc.logs++;
			sfxLog("spark chatter: CAR_SputOutput made {%d, %d, %d}", (int)p[0], (int)p[1], (int)p[2]);
		}
		if (mine >= 0) s_sc.out[mine] = NULL;   // a freed one's address, reused
		mine = -1;
		if (p[2] != SC_TAG) return;
		for (int i = 0; i < 4 && mine < 0; i++)
			if (!s_sc.out[i]) mine = i;
		if (mine < 0) mine = 0;
		s_sc.out[mine] = inst;
	}
	if (mine < 0) return;
	if (p[0] != s_sc.output && s_sc.logs < 24) {
		s_sc.logs++;
		sfxLog("spark chatter: output %d", (int)p[0]);
	}
	s_sc.output = p[0];
}

// SetupSFX + InitSFX.
static void scSetup(int engIdx)
{
	s_sc.swtn = &kSweetener[engIdx < 0x2c && engIdx >= 0 ? engIdx : 0];
	aemsWatch(aemsClass("CAR_SputOutput"), scOutput, NULL);
	memset(s_sc.data, 0, sizeof(s_sc.data));
	s_sc.data[0] = clampi(s_eng.eng ? s_eng.eng->id : 0, 0, 0x3c);   // CSTATE +0x7C's +8
	s_sc.data[1] = SC_TAG;
	s_sc.inst = aemsCreate(aemsClass("CAR_Sputter"), s_sc.data);
	s_sc.enabled = true;
	sfxLog("spark chatter: CAR_Sputter from %s, volume %d", s_sc.swtn->bank[0] ? s_sc.swtn->bank : "(no bank)",
	       (int)s_sc.swtn->vol2);
}

// UpdateParams: with a backfire pending and the car in neutral or first
// (CSTATE +0x50), the trigger field goes 0 -> 1 -- from 1, one frame at 0
// first, the flag kept till then. In any other gear it waits.
static void scUpdateParams(void)
{
	if (!s_sc.enabled || !s_sc.pending || !s_sc.inst) return;
	const int gear = enginectlMix()->gear;
	if (gear != 2 && gear != 1) return;
	if (s_sc.data[8] == 1) {
		s_sc.data[8] = 0;
	} else {
		s_sc.pending = false;
		s_sc.data[8] = 1;
		sfxLog("spark chatter: backfire (gear %d), volume %d at %d revs, %d voice(s) before",
		       gear, (int)s_sc.data[3], (int)s_sc.data[2], aemsVoicesPlaying());
	}
	aemsSetMemberData(s_sc.inst, s_sc.data);
}

// ProcessUpdate: the member data every frame, the volume the map's output 0
// over the sweetener's own (+8).
static void scProcessUpdate(const EngineCtlMix* m)
{
	if (!s_sc.inst) return;
	const int32_t vol = mixmapQ15(MIXMAP_SFX_SPARKCHATTER, 0) * s_sc.swtn->vol2 >> 15;
	s_sc.data[4] = clampi(mixmapAzim(MIXMAP_SFX_SPARKCHATTER, 0), 0, 0x10000);
	s_sc.data[5] = 0;
	s_sc.data[6] = 0;
	s_sc.data[2] = clampi((int32_t)m->sndRpm, 0, 10000);
	s_sc.data[7] = clampi((int32_t)(m->physTrq * 1024.0f * 0.01f), 0, 0x400);
	s_sc.data[3] = clampi(vol, 0, 0x8013);
	s_sc.data[10] = m->shiftState != 0;
	aemsSetMemberData(s_sc.inst, s_sc.data);
}

// UpdateMixerOutputs.
static void scUpdateMixerOutputs(void)
{
	int32_t* o = mixmapObjOut(MIXMAP_SFX_SPARKCHATTER);
	if (!o) return;
	if (s_sc.output != 0) {
		o[2] = s_sc.output;
		o[1] = 1;
	} else {
		o[1] = 0;
	}
}

// Destroy.
static void scShutdown(void)
{
	letGo(&s_sc.inst);
	aemsUnwatch(aemsClass("CAR_SputOutput"), scOutput);
	memset(&s_sc, 0, sizeof(s_sc));
}

// ---------------------------------------------------------------- turbo

// CARSFX_Turbo (ctor 0x800992F4, SetupSFX 0x800995C8, InitSFX 0x8009966C,
// UpdateParams 0x80099834, ProcessUpdate 0x800999C8, PlayBlowoff 0x80099C6C,
// IsBlowOffDone 0x80099F00, UpdateBlowOff 0x80099F80, StopBlowOff 0x8009A038,
// PlaySpl 0x8009A0B8, ResetSpool 0x8009A318, UpdateSpool 0x8009A328,
// SetupLoadData 0x800AEE08): the spool's whine, which climbs while the engine
// makes torque, and the blow-off valve's chatter when the throttle lets go
// (FX_TURBO_01, the turbo level's TURBO bank). A car whose physics has no
// boost curve has no turbo sound.
//
// Not ported: AdjustVolume, which silences it while the race coordinator is
// in its state 7 -- the port's races do not run the coordinator's states yet.
typedef struct {
	const TurboData* ds;   // +0xE0: kTurboData[CARDATAMAPPING[row][level][2]]
	AemsBank*  bank;       // the player's only
	bool       enabled;    // +0xA0: InitSFX found a boost curve
	int        level;      // +0xAC: GetUpgradeLevel(TURBO)
	AemsInst   spool;      // +0xBC's instance
	AemsInst   blow;       // +0xC0's instance
	int32_t    spoolData[6];   // 0, volume, boost, azimuth, rotation, revs
	int32_t    blowData[6];    // type, volume, boost, 0, rotation, revs
	InterpLine blowLine;   // +0xC4: the blow-off's fade (its cur at +0xD8)
	int32_t    spoolVol;   // +0xE4
	int32_t    blowVol;    // +0xE8
	int        blowType;   // +0xEC: 1, or 2..3 from a near-full spool
	float      spoolAmt;   // +0xF4: 0 .. ds->maxSpool
	float      boost;      // +0xF8: spoolAmt / maxSpool
	int        capped;     // +0x100: the spool is at its revs' cap
	PathLine   wobble;     // +0x104: the capped spool's slow fade (its cur at +0x188)
	int        state;      // +0x190: 0 off boost, 1 on it, 2 blowing off
	float      delta;      // +0x194: the boost this frame, torque / 100
	float      boostMax;   // +0x1C4: the boost curve's peak, x4
	uint32_t   sfxId;      // its object in the mix map
	char       who[8];     // debug: "" the player, "AI n " an opponent
} Turbo;

static Turbo     s_tb;
static float     s_tbReportIn;   // debug: every 5 s, where the spool stands
static uint32_t* s_rng;      // bRandom's state (main.c g_bRandom)
static uint32_t  s_rngOwn;   // ... or, without one, a private one (port guard)

// MIN_TORQUE_FOR_BLOWOFF (r13 - 0x77B8).
#define MIN_TORQUE_FOR_BLOWOFF 20.0f
// CSTATE +0xAC, which EAXCar::UpdateRotation (0x8007FE58) sets to 0x200.
#define CSTATE_ROTATION 0x200

static int32_t tbQ15(const Turbo* t, int output)  { return mixmapQ15(t->sfxId, output); }
static int32_t tbAzim(const Turbo* t, int output) { return mixmapAzim(t->sfxId, output); }

// PlaySpl(type, volume, boost, -, rotation): a new spool instance. Only
// UpdateSpool calls it, with every argument 0, when it has none.
static void turboPlaySpl(Turbo* t, const EngineCtlMix* m, int type, int32_t vol, int32_t boost,
	int32_t rot)
{
	vol = vol * tbQ15(t, 0) >> 15;
	letGo(&t->spool);
	t->spoolData[0] = clampi(type, 0, 5);
	t->spoolData[1] = clampi(vol, 0, 0x7fff);
	t->spoolData[2] = clampi(boost, 0, 0x2000);
	t->spoolData[3] = clampi(tbAzim(t, 0), 0, 0xffff);
	t->spoolData[4] = clampi(rot, 0, 0x400);
	t->spoolData[5] = clampi((int32_t)m->sndRpm, 0, 10000);
	t->spool = aemsCreate(aemsClass("FX_TURBO_01"), t->spoolData);
}

// PlayBlowoff: type 1, or past three quarters of the spool type 2 or 3 at
// random, at its level scaled by the spool. The fade line starts at 1.
static void turboPlayBlowoff(Turbo* t, const EngineCtlMix* m, int32_t rot)
{
	if (!t->enabled || t->blow) return;
	t->blowType = 1;
	if (0.75f < t->boost)
		t->blowType = (int)rideinfo_random(s_rng, 2) + 2;
	const int32_t v = t->blowType == 1 ? t->ds->blowVol1 : t->ds->blowVol2;
	t->blowVol = (int32_t)((float)v * t->boost);
	t->blowData[0] = clampi(t->blowType, 0, 5);
	t->blowData[1] = 0;
	t->blowData[2] = clampi((int32_t)(t->boost * 1024.0f), 0, 0x2000);
	t->blowData[3] = 0;
	t->blowData[4] = clampi(rot, 0, 0x400);
	t->blowData[5] = clampi((int32_t)m->sndRpm, 0, 10000);
	t->blow = aemsCreate(aemsClass("FX_TURBO_01"), t->blowData);
	lineInit(&t->blowLine, 1.0f, 1.0f, 1, 0);
	sfxLog("%sturbo: blow-off type %d, level %d (boost %.2f)", t->who, t->blowType,
	       (int)t->blowVol, t->boost);
}

static bool turboBlowOffDone(const Turbo* t)
{
	if (!t->blow) return true;
	return aemsRefCount(t->blow) == 1 || t->blowLine.cur == 0.0f;
}

// UpdateBlowOff: back on the throttle, out of any shift, the blow-off fades
// out over 150 ms; done, it is let go (StopBlowOff).
static void turboUpdateBlowOff(Turbo* t, const EngineCtlMix* m, float dt)
{
	if (!t->blow) return;
	lineUpdate(&t->blowLine, dt);
	if (MIN_TORQUE_FOR_BLOWOFF < m->thrPct && t->blowLine.cur == 1.0f && m->shiftState == 0)
		lineInit(&t->blowLine, 1.0f, 0.0f, 150, 0);
	if (turboBlowOffDone(t)) letGo(&t->blow);
}

// UpdateSpool: the spool fills by the boost and drains by the bank's decay,
// capped by the revs (40 % of full at idle, all of it from 1100); pinned at
// full it fades along a quarter cosine.
static void turboUpdateSpool(Turbo* t, const EngineCtlMix* m, float dt, bool paused)
{
	if (!t->spool) {
		turboPlaySpl(t, m, 0, 0, 0, 0);
		return;
	}
	const TurboData* ds = t->ds;
	if (!paused)
		t->spoolAmt = t->delta <= 0.01f ? t->spoolAmt - ds->decay : t->spoolAmt + t->delta;
	float v = t->spoolAmt;
	if (v < 0.0f) v = 0.0f;
	if (ds->maxSpool < v) v = ds->maxSpool;
	t->spoolAmt = v;
	float f = (float)((int32_t)m->sndRpm - 1000) / 100.0f;
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	const float k = f * 0.6f + 0.4f;
	if (ds->maxSpool * k < v) v = ds->maxSpool * k;
	t->spoolAmt = v;
	if (v == ds->maxSpool * k && !t->capped) {
		t->capped = 1;
		pathClear(&t->wobble);
		pathAdd(&t->wobble, 0.0f, 0.0f, 1000, 0);
		pathAdd(&t->wobble, 0.0f, 307.2f, 2000, 0);
	} else if (t->spoolAmt != ds->maxSpool && t->capped) {
		t->capped = 0;
	}
	t->boost = t->spoolAmt / ds->maxSpool;
	t->spoolVol = (int32_t)(t->boost * 32767.0f) * ds->spoolVol >> 15;
	if (t->capped) {
		pathUpdate(&t->wobble, dt);
		t->spoolVol = t->spoolVol * kCosTable[(int32_t)t->wobble.cur] >> 15;
	}
}

// UpdateParams: the boost is the engine's torque over 20 (of 100); falling
// back under 20 blows off and empties the spool.
static void turboUpdateParams(Turbo* t, const EngineCtlMix* m, float dt, bool paused)
{
	if (!t->enabled) return;
	t->delta = m->torque <= MIN_TORQUE_FOR_BLOWOFF ? 0.0f : m->torque / 100.0f;
	turboUpdateBlowOff(t, m, dt);
	turboUpdateSpool(t, m, dt, paused);
	switch (t->state) {
	case 0:
		if (MIN_TORQUE_FOR_BLOWOFF < m->torque) t->state = 1;
		break;
	case 1:
		if (m->torque < MIN_TORQUE_FOR_BLOWOFF) {
			t->state = 2;
			turboPlayBlowoff(t, m, CSTATE_ROTATION);
			t->spoolAmt = 0.0f;   // ResetSpool
			turboUpdateSpool(t, m, dt, paused);
		}
		break;
	case 2:
		if (turboBlowOffDone(t)) t->state = 0;
		break;
	}
}

// ProcessUpdate: this frame's volumes off the mix map (the blow-off's type 1
// on output 1, types 2 and 3 on 2; the spool on 0), the boost and the revs.
static void turboProcessUpdate(Turbo* t, const EngineCtlMix* m)
{
	if (t->blow) {
		const int32_t q = tbQ15(t, t->blowType == 1 ? 1 : 2);
		const int32_t vol = (int32_t)((float)(t->blowVol * q >> 15) * t->blowLine.cur);
		t->blowData[1] = clampi(vol, 0, 0x7fff);
		aemsSetMemberData(t->blow, t->blowData);
	}
	if (t->spool) {
		t->spoolData[1] = clampi(t->spoolVol * tbQ15(t, 0) >> 15, 0, 0x7fff);
		t->spoolData[2] = clampi((int32_t)(t->boost * 1024.0f), 0, 0x2000);
		t->spoolData[3] = clampi(tbAzim(t, 0), 0, 0xffff);
		t->spoolData[4] = clampi(CSTATE_ROTATION, 0, 0x400);
		t->spoolData[5] = clampi((int32_t)m->sndRpm, 0, 10000);
		aemsSetMemberData(t->spool, t->spoolData);
	}
}

// SetupSFX and InitSFX: the turbo level's dataset (GetUpgradeLevel(TURBO) of
// the car's own DriverInfo), and the turbo on only when the car's boost curve
// (CarPhysicsInfo +0x224, nine points) rises above 0.
static void turboInit(Turbo* t, int row, int level, const float* curve)
{
	t->level = level;
	t->ds = &kTurboData[kCarDataMapping[row][clampi(level, 0, 3)][2]];
	t->state = 0;
	float peak = 0.0f;
	for (int i = 0; i < 9; i++)
		if (peak < curve[i]) peak = curve[i];
	t->boostMax = peak * 4.0f;
	t->delta = 0.0f;
	t->enabled = t->boostMax != 0.0f;
	if (t->enabled) {
		// The Average(3) at +0x198 InitSFX flushes nothing reads.
		t->capped = 0;
		t->state = 0;
		t->blow = t->spool = NULL;
	}
	sfxLog("%sturbo level %d -> %s: boost peak %.3f, %s", t->who, level, t->ds->bank, peak,
	       t->enabled ? "on" : "no boost curve, off");
}

// The player's: SetupSFX, SetupLoadData (TURBO) and InitSFX.
static void turboSetup(const char* audioDir, int row, int level, const float* curve, AudioBus bus)
{
	char path[512], name[64];
	s_tb.sfxId = MIXMAP_SFX_TURBO;
	turboInit(&s_tb, row, level, curve);
	snprintf(name, sizeof(name), "%s", s_tb.ds->bank);
	char* dot = strrchr(name, '.');
	if (dot) *dot = '\0';
	snprintf(path, sizeof(path), "%sTURBO/%s.aem", audioDir, name);
	s_tb.bank = aemsLoad(path, bus);
	if (!s_tb.bank)
		sfxLog("turbo: no %s -- the turbo silent (tools/abk3ds.py)", path);
}

static void turboShutdown(Turbo* t)
{
	letGo(&t->spool);
	letGo(&t->blow);
	if (t->bank) aemsUnload(t->bank);
	memset(t, 0, sizeof(*t));
}

// ---------------------------------------------------------------- skids
//
// The tyres' screech, in three layers a side -- spin (a wheel turning faster
// than the road), slide (the car sideways to its travel) and lock (a braked
// wheel dragged) -- off two controllers and one SFX of the car's state:
//
//   SFXCTL_Wheel (1)   each side's tyre slip and grip, its terrain, and two
//                      points ahead of the car's sides for the map's 3D
//                      wheel positions (SFXCTL_3DRight/LeftWheelPos).
//   SFXCTL_Skid  (2)   GenerateSkidNoise: the six levels, 0..1024, from those.
//                      Its outputs are the two sides' mean levels and their
//                      peak x 32, which the mix map shapes into the skids'
//                      master channel.
//   CARSFX_Skids (5)   one FX_SKID instance a side, which the levels, the
//                      map's volume and azimuth, the yaw and speed drive.
//
// "Right" is wheels 1 and 2, "left" wheels 0 and 3, as SFXCTL_Wheel sums them.
// CARSFX_Skids' voice 0 is the left side, voice 1 the right.
//
// An opponent's CARSFX_Skids has the one voice SetupSFX gives an AI driver
// (DriverInfo +4 == 2), on the two sides' mean levels; its map reads neither
// controller, and has no wheel positions.
//
// Not ported: GenerateTerrainTypes' METAL_GRATE (0x19) case, which sets both
// sides to it in game mode 8 with a state the port has no counterpart of; and
// the remote player's one voice (DriverInfo +0x8E0), which a single-player
// game never has.

// smooth<int> (0x80302E50): toward `target`, at most `up` higher or `down`
// lower than `prev`.
static int32_t smoothi(int32_t prev, int32_t target, int32_t up, int32_t down)
{
	if (prev + up < target) return prev + up;
	if (target < prev - down) return prev - down;
	return target;
}

// Slope (Initialize 0x80085404, Regenerate 0x800854E0) from 0 at x0 to 1 at
// x1, flat either side.
static float slopeValue(float x0, float x1, float x)
{
	if (fabsf(x1 - x0) < 1e-6f) x1 += 1e-6f;
	float t = (x - x0) / (x1 - x0);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return t;
}

typedef struct {
	// SFXCTL_Wheel
	float   rSlip[2];     // +0x90 / +0x94: wheels 1 and 2's slip pairs, summed, those down
	float   lSlip[2];     // +0x98 / +0x9C: wheels 0 and 3's
	float   grip[4];      // +0xA0: |grip use| by wheel
	int     terrain[2];   // +0xB0 left, +0xB4 right
	int     lDown, rDown; // +0xE8 / +0xEC: a wheel of the side is on the ground
	// SFXCTL_Skid
	const SkidParams* p;  // +0x6C
	float   spin[2];      // +0x8C right, +0x90 left
	float   lock[2];      // +0x9C right, +0xA0 left
	Average spinAvg[2];   // +0xA4 right, +0xCC left, of 5 (sfxline.h)
	Average lockAvg[2];   // +0x144 right, +0x16C left, of 5
	int32_t mag[2][3];    // +0x194: [0] the right side's spin, slide, lock; [1] the left's
	float   timer[2];     // +0x1AC / +0x1B0: how long the side's grip has been past the limit
	int32_t over[2];      // +0x1B4 / +0x1B8: how far past, 0..1024
	int32_t slide[2];     // +0x1BC / +0x1C0
	int32_t back[2];      // +0x1C4 / +0x1C8
	// CARSFX_Skids
	AemsBank* bank;       // the player's only
	bool      enabled;    // +0xA0: InitSFX ran
	int       voices;     // +0xC4: 2 the player's, 1 an opponent's
	int       drift;      // r13 - 0x7798: SetupLoadData's drift flag
	AemsInst  voice[2];   // +0xB0
	int32_t   scale[2][3];    // +0xC8: each layer's level for the side's terrain
	int32_t   mags[2][3];     // +0xF8: GetSkidMag(voice, layer, voices)
	int32_t   data[2][17];    // FX_SKID's member data
	int32_t   lastPeak;   // debug: the controller's peak output last logged
	uint32_t  sfxId;      // CARSFX_Skids' object in the mix map
	uint32_t  ctlId;      // SFXCTL_Skid's (an opponent's is not in it)
	char      who[8];     // debug: "" the player, "AI n " an opponent
} Skids;

static Skids s_sk;
static float s_skReportIn;   // debug: every 5 s, the levels
static float s_skEventIn;    // debug: at most once a second, a skid starting or ending

static int32_t skQ15(const Skids* sk, int output)  { return mixmapQ15(sk->sfxId, output); }
static int32_t skAzim(const Skids* sk, int output) { return mixmapAzim(sk->sfxId, output); }

// g_IsTruck (eSND_CAR_TYPES 9, 0xD, 0xE, 0x14, 0x15, the SUVs), the same rows
// as GetCartypeEngGroup's (0x8007EC2C) group 2, which picks the bank.
static bool isTruck(int row)
{
	return row == 0x9 || row == 0xd || row == 0xe || row == 0x14 || row == 0x15;
}

// SFXCTL_Wheel::UpdateTireParams: GenerateWheelPosition (the player's only:
// the AICar state's map has no wheel positions), GenerateTerrainTypes, then
// each side's slip summed over its wheels on the ground.
static void wheelUpdate(Skids* sk, const CarSfxIn* in)
{
	if (sk == &s_sk) {
		float lPos[3], rPos[3];
		for (int j = 0; j < 3; j++) {
			lPos[j] = in->pos[j] + in->left[j] * 0.8f + in->fwd[j] * 1.5f;
			rPos[j] = in->pos[j] - in->left[j] * 0.8f + in->fwd[j] * 1.5f;
		}
		mixmapSetObjPos(MIXMAP_CTL_3DLWHEELPOS, lPos);
		mixmapSetObjPos(MIXMAP_CTL_3DRWHEELPOS, rPos);
	}
	sk->lDown = in->wheel[0].onGround || in->wheel[3].onGround;
	sk->rDown = in->wheel[1].onGround || in->wheel[2].onGround;
	// A side's terrain changes only when both its wheels agree.
	if (in->wheel[0].terrain == in->wheel[3].terrain) sk->terrain[0] = in->wheel[0].terrain;
	if (in->wheel[1].terrain == in->wheel[2].terrain) sk->terrain[1] = in->wheel[1].terrain;
	for (int j = 0; j < 2; j++) sk->rSlip[j] = sk->lSlip[j] = 0.0f;
	for (int i = 0; i < 4; i++) {
		sk->grip[i] = fabsf(in->wheel[i].gripUse);
		if (!in->wheel[i].onGround) continue;
		float* side = (i == 1 || i == 2) ? sk->rSlip : sk->lSlip;
		side[0] += in->wheel[i].slip[0];
		side[1] += in->wheel[i].slip[1];
	}
}

// GetTerrainType(voice, 2): the voice's side.
static int skidTerrain(const Skids* sk, int voice)
{
	return (voice & 1) ? sk->terrain[1] : sk->terrain[0];
}

// GetSkidMag(voice, layer, voices): two voices, voice 0 the left side and 1
// the right; one, the mean of the two.
static int32_t skidMag(const Skids* sk, int voice, int layer, int voices)
{
	if (layer < 0 || layer > 2) return 0;
	if (voices == 1) return (sk->mag[0][layer] + sk->mag[1][layer]) >> 1;
	if (voices != 2 || voice < 0 || voice > 1) return 0;
	return sk->mag[voice == 0 ? 1 : 0][layer];
}

// SFXCTL_Skid::GenerateSkidNoise for one side's grip: past the limit on
// either wheel for longer than gripTime, how far past it, 0..1024.
static int32_t skidOver(Skids* sk, int side, float gA, float gB, float speed, float dt)
{
	const SkidParams* p = sk->p;
	if (0.5f < fabsf(speed)) {
		if (p->gripLimit < gA || p->gripLimit < gB) {
			sk->timer[side] += dt;
			if (p->gripTime < sk->timer[side]) {
				const float g = gB < gA ? gA : gB;
				return (int32_t)((g - p->gripLimit) * 1024.0f / (1.0f - p->gripLimit));
			}
		} else {
			sk->timer[side] = 0.0f;
		}
	}
	return 0;
}

static float clampf(float v, float lo, float hi)
{
	if (v - lo < 0.0f) v = lo;
	if (hi - v < 0.0f) v = hi;
	return v;
}

// SFXCTL_Skid::GenerateSkidNoise: the six levels.
static void skidGenerate(Skids* sk, const CarSfxIn* in, float dt)
{
	const SkidParams* p = sk->p;
	const float* R = sk->rSlip;
	const float* L = sk->lSlip;
	// Spin: a side's forward slip, while its tyres turn faster than the road.
	sk->spin[0] = clampf(0.0f < R[0] ? R[0] * p->spinScale : 0.0f, p->spinMin, p->spinMax);
	sk->spin[1] = clampf(0.0f < L[0] ? L[0] * p->spinScale : 0.0f, p->spinMin, p->spinMax);
	avgRecord(&sk->spinAvg[0], sk->spin[0]);
	avgRecord(&sk->spinAvg[1], sk->spin[1]);
	const float spinSpan = p->spinMax - p->spinMin;
	sk->mag[0][0] = (int32_t)((sk->spinAvg[0].avg - p->spinMin) * 1024.0f / spinSpan);
	sk->mag[1][0] = (int32_t)((sk->spinAvg[1].avg - p->spinMin) * 1024.0f / spinSpan);
	// How far past their grip each side's tyres are.
	sk->over[0] = skidOver(sk, 0, sk->grip[1], sk->grip[2], in->speed, dt);
	sk->over[1] = skidOver(sk, 1, sk->grip[0], sk->grip[3], in->speed, dt);
	// The travel in the car's frame, normalised (the matrix turns it without
	// changing its length): forwards and sideways.
	float v[2] = { 0.0f, 0.0f };
	for (int k = 0; k < 3; k++) {
		v[0] += in->vel[k] * in->fwd[k];
		v[1] += in->vel[k] * in->left[k];
	}
	const float vv = in->vel[0] * in->vel[0] + in->vel[1] * in->vel[1] + in->vel[2] * in->vel[2];
	const float len = vv > 0.0f ? sqrtf(vv) : 0.0f;
	if (len > 0.0f) {
		v[0] /= len;
		v[1] /= len;
	}
	const float lat = fabsf(v[1]);
	const float back = v[0] <= 0.0f ? fabsf(v[0]) : 0.0f;
	// Slide: the sideways part of the grip, plus the yaw's own share.
	const float yaw = in->yaw;
	const float sv = slopeValue(p->yawLo, p->yawHi, fabsf(yaw));
	const float f = lat * p->slideMix;
	const float k = sv * (1.0f - p->slideMix);
	sk->slide[0] = smoothi(sk->slide[0], (int32_t)(f * (float)sk->over[0]), p->slideUp, p->slideDown);
	sk->slide[1] = smoothi(sk->slide[1], (int32_t)(f * (float)sk->over[1]), p->slideUp, p->slideDown);
	float r1 = k * 1024.0f + (float)sk->slide[0];
	if (yaw <= 0.0f) r1 = p->yawSide * r1;
	float l1 = k * 1024.0f + (float)sk->slide[1];
	if (0.0f < yaw) l1 = p->yawSide * l1;
	sk->mag[0][1] = (int32_t)r1;
	sk->mag[1][1] = (int32_t)l1;
	// Lock: a side's sideways slip while its tyres turn slower than the road.
	sk->lock[0] = clampf(R[0] < 0.0f ? fabsf(R[1] * p->lockScale) : 0.0f, p->lockMin, p->lockMax);
	sk->lock[1] = clampf(L[0] < 0.0f ? fabsf(L[1] * p->lockScale) : 0.0f, p->lockMin, p->lockMax);
	const float lockSpan = p->lockMax - p->lockMin;
	avgRecord(&sk->lockAvg[0], (float)(int32_t)((sk->lock[0] - p->lockMin) * 1024.0f / lockSpan));
	avgRecord(&sk->lockAvg[1], (float)(int32_t)((sk->lock[1] - p->lockMin) * 1024.0f / lockSpan));
	int32_t lockR = (int32_t)sk->lockAvg[0].avg, lockL = (int32_t)sk->lockAvg[1].avg;
	// ... mixed with sliding backwards, and only on the brakes.
	const float braking = in->brake < in->ebrake ? in->ebrake : in->brake;
	const float b = back * p->backMix;
	int32_t backR = (int32_t)(b * (float)sk->over[0]);
	int32_t backL = (int32_t)(b * (float)sk->over[1]);
	if (braking <= 0.15f) {
		lockR = lockL = 0;
		backR = backL = 0;
	}
	sk->back[0] = smoothi(sk->back[0], backR, p->backUp, p->backDown);
	sk->back[1] = smoothi(sk->back[1], backL, p->backUp, p->backDown);
	sk->mag[0][2] = (int32_t)((float)lockR * (1.0f - p->backMix)) + (int32_t)((float)sk->back[0] * p->backMix);
	sk->mag[1][2] = (int32_t)((float)lockL * (1.0f - p->backMix)) + (int32_t)((float)sk->back[1] * p->backMix);
	for (int s = 0; s < 2; s++)
		for (int l = 0; l < 3; l++)
			sk->mag[s][l] = clampi(sk->mag[s][l], 0, 0x400);
}

// SFXCTL_Skid::UpdateParams: the levels, and the outputs the map reads -- the
// sides' mean of each layer, and the loudest of those x 32.
static void skidCtlUpdate(Skids* sk, const CarSfxIn* in, float dt)
{
	skidGenerate(sk, in, dt);
	int32_t* o = mixmapObjOut(sk->ctlId);
	if (!o) return;
	o[0] = skidMag(sk, 0, 0, 1);
	o[1] = skidMag(sk, 0, 1, 1);
	o[2] = skidMag(sk, 0, 2, 1);
	int32_t peak = o[0] < o[1] ? o[1] : o[0];
	if (peak < o[2]) peak = o[2];
	o[3] = clampi(peak << 5, 0, 0x7fff);
}

// CalculateUndersteerFactor (0x80224F00): the front tyres' sideways slip
// against the steering, over the speed. CalculateOversteerFactor
// (0x80224FB4): the rear's. CARSFX_Skids takes both as ints.
static float understeer(const CarSfxIn* in)
{
	const float front = (in->wheel[0].slip[1] + in->wheel[1].slip[1]) * 0.5f;
	if (0.0f < in->vfwd && 1.0f < in->speed && in->steerDeg * front < 0.0f)
		return fabsf(front) / in->speed;
	return 0.0f;
}

static float oversteer(const CarSfxIn* in)
{
	if (in->vfwd <= 0.0f || in->speed <= 1.0f) return 0.0f;
	return fabsf((in->wheel[3].slip[1] + in->wheel[2].slip[1]) * 0.5f) / in->speed;
}

// CARSFX_Skids::UpdateParams: each voice's level for its side's terrain --
// grass, deep water and plants quieter -- nothing for a side with no wheel
// down, and its three layers.
static void skidsUpdateParams(Skids* sk)
{
	if (!sk->enabled) return;
	for (int i = 0; i < sk->voices && i < 2; i++) {
		const int t = skidTerrain(sk, i);
		const int32_t sc = (t < 0xe || (0xf < t && t != 0x16)) ? 0x7fff : SKID_QUIET_TERRAIN_TYPE_SCALE;
		for (int l = 0; l < 3; l++) sk->scale[i][l] = sc;
		// CSTATE +0x14: the PlayerCar state (1) silences either side with no
		// wheel down, the AICar state (2) its one voice while the left side
		// has none.
		const bool down = sk == &s_sk ? (i == 0 ? sk->lDown : sk->rDown) : sk->lDown;
		if (!down)
			for (int l = 0; l < 3; l++) sk->scale[i][l] = 0;
		for (int l = 0; l < 3; l++) sk->mags[i][l] = skidMag(sk, i, l, sk->voices);
	}
}

// CARSFX_Skids::ProcessUpdate: each voice's three volumes off its side of the
// mix map's six outputs (the left on 3 / 5 / 4, the right on 0 / 2 / 1), its
// azimuth, the levels, the yaw in degrees, the speed in mph, the under- and
// oversteer, and whether the side is on dirt or gravel.
static void skidsProcessUpdate(Skids* sk, const CarSfxIn* in)
{
	if (!sk->enabled) return;
	static const int8_t kOut[2][3] = { { 3, 5, 4 }, { 0, 2, 1 } };
	static const int8_t kAzim[2] = { 3, 1 };
	for (int i = 0; i < sk->voices && i < 2; i++) {
		if (!sk->voice[i]) continue;
		int32_t* d = sk->data[i];
		for (int l = 0; l < 3; l++)
			d[l] = clampi(sk->scale[i][l] * skQ15(sk, kOut[i][l]) >> 15, 0, 0x7fff);
		d[6] = clampi(skAzim(sk, kAzim[i]), 0, 0xffff);
		d[16] = (uint32_t)(skidTerrain(sk, i) - 8) < 2;
		d[3] = clampi(sk->mags[i][0], 0, 0x400);
		d[4] = clampi(sk->mags[i][1], 0, 0x400);
		d[5] = clampi(sk->mags[i][2], 0, 0x3ff);
		d[11] = clampi((int32_t)(fabsf(in->yaw) * 57.2957763671875f), 0, 180);
		d[12] = clampi((int32_t)(in->speed * 2.237f), 0, 1000);
		d[14] = clampi((int32_t)understeer(in), 0, 0x400);
		d[15] = clampi((int32_t)oversteer(in), 0, 0x400);
		aemsSetMemberData(sk->voice[i], d);
	}
}

// SFXCTL_Skid::SetupCTL / InitCTL, and CARSFX_Skids' SetupSFX, InitSFX and
// InitSkids: the car's own skid tuning (a car's or a truck's, the drift one
// in a drift race), `voices` FX_SKID instances (2 for the player, 1 for an
// opponent), every level 0, the layers' pitches at 0x1000. InitCTL flushes
// the right side's spin average twice and the left's never, so the left one
// starts empty. Without the player's bank there are no voices.
static void skidsInit(Skids* sk, int row, bool drift, int voices, uint32_t sfxId, uint32_t ctlId,
	const char* who)
{
	AemsBank* bank = sk->bank;   // the player's, loaded first
	memset(sk, 0, sizeof(*sk));
	sk->bank = bank;
	sk->sfxId = sfxId;
	sk->ctlId = ctlId;
	sk->voices = voices;
	snprintf(sk->who, sizeof(sk->who), "%s", who);
	sk->drift = drift ? 1 : 0;
	const bool truck = isTruck(row);
	sk->p = &kSkidParams[(truck ? 2 : 0) + sk->drift];
	for (int s = 0; s < 2; s++) {
		avgInit(&sk->spinAvg[s], 5);
		avgInit(&sk->lockAvg[s], 5);
		avgFlush(&sk->lockAvg[s], 0.0f);
	}
	avgFlush(&sk->spinAvg[0], 0.0f);
	if (!s_sk.bank) return;
	const AemsClass cls = aemsClass("FX_SKID");
	for (int i = 0; i < voices && i < 2; i++) {
		int32_t* d = sk->data[i];
		memset(d, 0, sizeof(sk->data[i]));
		d[7] = clampi(sk->drift, 0, 3);
		d[8] = d[9] = d[10] = 0x1000;
		sk->voice[i] = aemsCreate(cls, d);
	}
	sk->enabled = true;
	sfxLog("%sskids: %s%s, %d voice(s) %s/%s", sk->who, truck ? "truck" : "car", drift ? ", drift" : "",
	       voices, sk->voice[0] ? "ok" : "none", voices > 1 ? (sk->voice[1] ? "ok" : "none") : "-");
}

// The player's, with SetupLoadData (SKIDS): a car's or a truck's bank, the
// drift ones only in a drift race, which the opponents' play through too.
static void skidsSetup(const char* audioDir, int row, bool drift, AudioBus bus)
{
	static const char* const kSkidBank[4] = {
		"SKID_PAV_MB", "SKID_PAV2_MB", "SKIDS_DRIFT_MB", "SKIDS_DRIFT2_MB"
	};
	char path[512];
	memset(&s_sk, 0, sizeof(s_sk));
	const char* name = kSkidBank[(isTruck(row) ? 1 : 0) + (drift ? 2 : 0)];
	snprintf(path, sizeof(path), "%sSKIDS/%s.aem", audioDir, name);
	s_sk.bank = aemsLoad(path, bus);
	if (!s_sk.bank)
		sfxLog("skids: no %s -- the tyres silent (tools/abk3ds.py)", path);
	else
		sfxLog("skids: %s", name);
	skidsInit(&s_sk, row, drift, 2, MIXMAP_SFX_SKIDS, MIXMAP_CTL_SKID, "");
}

static void skidsShutdown(Skids* sk)
{
	letGo(&sk->voice[0]);
	letGo(&sk->voice[1]);
	if (sk->bank) aemsUnload(sk->bank);
	memset(sk, 0, sizeof(*sk));
}

// Debug: the levels every 5 s, and a skid starting or ending (at most once a
// second).
static void skidsReport(const CarSfxIn* in, float dt)
{
	if (!s_sk.enabled) return;
	const int32_t* o = mixmapObjOut(MIXMAP_CTL_SKID);
	const int32_t peak = o ? o[3] : 0;
	s_skEventIn -= dt;
	const bool change = (peak > 0) != (s_sk.lastPeak > 0);
	if ((s_skReportIn -= dt) <= 0.0f || (change && s_skEventIn <= 0.0f)) {
		if (s_skReportIn <= 0.0f) s_skReportIn = 5.0f;
		s_skEventIn = 1.0f;
		s_sk.lastPeak = peak;
		sfxLog("skids: L %d/%d/%d R %d/%d/%d peak %d; grip %.2f %.2f %.2f %.2f, over %d/%d, "
		       "yaw %.1f deg, %.1f m/s; vol L %d/%d/%d R %d/%d/%d az %04x/%04x",
		       (int)s_sk.mag[1][0], (int)s_sk.mag[1][1], (int)s_sk.mag[1][2],
		       (int)s_sk.mag[0][0], (int)s_sk.mag[0][1], (int)s_sk.mag[0][2], (int)peak,
		       s_sk.grip[0], s_sk.grip[1], s_sk.grip[2], s_sk.grip[3],
		       (int)s_sk.over[1], (int)s_sk.over[0], in->yaw * 57.29578f, in->speed,
		       (int)s_sk.data[0][0], (int)s_sk.data[0][1], (int)s_sk.data[0][2],
		       (int)s_sk.data[1][0], (int)s_sk.data[1][1], (int)s_sk.data[1][2],
		       (unsigned)s_sk.data[0][6], (unsigned)s_sk.data[1][6]);
	}
}

// ---------------------------------------------------------------- road / wind
//
// The tyres rolling on the road and the air past the car, the PlayerCar
// state's SFX 6 and 7, out of the two IG_GLOBAL banks:
//
//   CARSFX_RoadNoise (6)  a side's loop for the terrain under it
//                         (FX_ROADNOISE, sample LoopID(terrain)), its rumble
//                         (FX_ROADNOISE, sample 4), and a one-shot as it runs
//                         on to or off a terrain (FX_ROADNOISE_TRANS).
//                         GenerateRoadNoise sets the levels and pitches from
//                         the speed and SFXCTL_Wheel's slip and grip (the
//                         skids' wheel controller, above).
//   CARSFX_WindNoise (7)  one FX_WIND, louder with the speed.
//   CARSFX_WindWeather (10) one FX_WIND_Weather out of the same WIND_00_MB,
//                         the weather's wind: loudest at a standstill.
//
// Road noise's arrays go by retail's `left` flag: [0] the right side, [1] the
// left. Its mix map outputs are 0 / 1 the right / left loop, 2 / 3 their
// transitions, 4 / 5 their rumbles and 6 / 7 their stitches.
//
// A transition also starts a stitch on its side when the side has none
// playing (cStichWrapper, type 0, ids 0x4E..0x51), through the collisions'
// stitch player (colsfx.h). The terrain is the topology's under each wheel
// (carsim.h, Wheel::CheckTerrain).
//
// Not ported: the wind's two 3D points (SFXCTL_3DLeft/RightWindPos), which
// MAPOUTPUT never reads.

// RoadNoiseVolumes (0x80355E1C): each loop's level.
static const int32_t kRoadNoiseVolumes[8] = { 15000, 24000, 32000, 24000, 24000, 5000, 19000, 32500 };

typedef struct {
	AemsBank* bank;          // IG_GLOBAL/ROADNOISE_00_MB
	bool      enabled;       // +0xA0: InitSFX ran
	AemsInst  rumble[2];     // +0xB8 / +0xBC
	AemsInst  loop[2];       // +0xC0 / +0xC4
	AemsInst  trans[2];      // +0xC8 / +0xCC
	ColsfxStitch* stitch[2]; // +0xD0 / +0xD4, the transition's cStichWrapper
	int32_t   rumbleData[2][7];
	int32_t   loopData[2][7];
	int32_t   transData[2][7];
	int       terrain[2];    // +0xDC / +0xD8
	int       oldTerrain[2]; // +0xE4 / +0xE0
	bool      changed[2];    // +0xEC / +0xE8
	int       loopId[2];     // +0xF8 / +0xFC
	int32_t   vol[2];       // +0x100 / +0x104
	int32_t   pitch[2];      // +0x108 / +0x10C
	float     mph;           // Car +0x440 x 2.237, as GenerateRoadNoise read it
} RoadNoise;

typedef struct {
	AemsBank* bank;          // IG_GLOBAL/WIND_00_MB
	bool      enabled;
	AemsInst  wind;          // FX_WIND
	int32_t   data[7];
	int32_t   vol;           // +0x18C: speed / 40 x 32767
	int32_t   f198;          // +0x198: speed / 40 x 1040, member [6]
} WindNoise;

typedef struct {
	AemsInst  inst;          // +0xB8: FX_WIND_Weather
	int32_t   data[4];       // its member data, +0xB8 + 4
	float     vol;           // +0xB4: WeatherWindVolSlope of the speed
} WindWeather;

static RoadNoise   s_rn;
static WindNoise   s_wn;
static WindWeather s_ww;
static float     s_rwReportIn;   // debug: every 5 s, the levels
static int       s_rnTransLogs;  // debug: the first 40 transitions are logged
// PlayTransition's stitch counts, r13 - 0x77AC (right) and - 0x77B0 (left):
// globals, so they carry on from one world to the next.
static int       s_rnStitchCount[2];

static int32_t rnQ15(int output)  { return mixmapQ15(MIXMAP_SFX_ROADNOISE, output); }
static int32_t rnAzim(int output) { return mixmapAzim(MIXMAP_SFX_ROADNOISE, output); }
static int32_t wnQ15(int output)  { return mixmapQ15(MIXMAP_SFX_WINDNOISE, output); }
static int32_t wnAzim(int output) { return mixmapAzim(MIXMAP_SFX_WINDNOISE, output); }
static int32_t wwQ15(int output)  { return mixmapQ15(MIXMAP_SFX_WINDWEATHER, output); }

// Slope::GetValue (0x80085488): y0 at x0 to y1 at x1, flat either side.
static float slopeY(float y0, float y1, float x0, float x1, float x)
{
	return y0 + slopeValue(x0, x1, x) * (y1 - y0);
}

// Graph::GetValue (0x801A5214) over road noise's speed-to-level graph
// (r13 - 0x5220): straight between its points, flat past either end.
static float rnSpeedToVol(float mph)
{
	static const float k[5][2] = {
		{ 0.0f, 0.0f }, { 60.0f, 28000.0f }, { 100.0f, 32500.0f }, { 150.0f, 24500.0f }, { 175.0f, 18000.0f }
	};
	if (mph <= k[0][0]) return k[0][1];
	for (int i = 1; i < 5; i++)
		if (mph < k[i][0])
			return k[i - 1][1] + (mph - k[i - 1][0]) * (k[i][1] - k[i - 1][1]) / (k[i][0] - k[i - 1][0]);
	return k[4][1];
}

// GetSndLoopIDForTerrainType (0x8009BA28).
static int rnLoopId(int t)
{
	switch (t) {
	case 8: case 9: case 10: case 15: return 0;
	case 16:                          return 1;
	case 11: case 12: case 17:        return 2;
	case 14:                          return 3;
	case 2: case 13:                  return 4;
	case 1: case 3:                   return 5;
	case 25:                          return 6;
	case 7: case 26:                  return 7;
	default:                          return -1;
	}
}

// GetSndOnToTransitionIDForTerrainType (0x8009BB04) and
// GetSndOffOfTransitionIDForTerrainType (0x8009BB50).
static int rnOnTo(int t)  { return t == 11 ? 0 : t == 14 ? 2 : t == 16 ? 0 : -1; }
static int rnOffOf(int t) { return (t == 11 || t == 16) ? 1 : -1; }

// GetRoadNoiseTransitionVol (0x800880A8).
static int32_t rnTransitionVol(int id)
{
	return (id == 0 || id == 1) ? 0x3c8c : id == 2 ? 26000 : 0x7fff;
}

// The length of a side's slip pair: frsqrte and two Newton steps over a sum
// of squares past 5e-11, else 0.
static float rnSlipLen(const float* s)
{
	const float q = s[0] * s[0] + s[1] * s[1];
	return q > 5e-11f ? sqrtf(q) : 0.0f;
}

static float rnMin(float v, float hi) { return v > hi ? hi : v; }

// CARSFX_RoadNoise::GenerateRoadNoise (0x8009D0F0). The graph's level goes
// through a unity master (x 0x7fff >> 15); the right side's level has an
// x1.1 where the left's has its grip, and both sides' pitches have their own
// grip.
static void rnGenerate(const CarSfxIn* in)
{
	s_rn.mph = in->speed * 2.237f;
	const int32_t g = (int32_t)rnSpeedToVol(s_rn.mph);
	const float base = (float)(g * 0x7fff >> 15);
	const float rS = rnSlipLen(s_sk.rSlip), lS = rnSlipLen(s_sk.lSlip);
	const float* gr = s_sk.grip;
	float volR = base * (1.0f + rnMin(rS * 0.01f, 0.15f));
	volR = volR * 0.1f + volR;
	float volL = base * (1.0f + rnMin(lS * 0.01f, 0.15f));
	volL *= 1.0f + rnMin((gr[0] + gr[3]) * 0.5f * 0.1f, 0.1f);
	s_rn.vol[0] = (int32_t)rnMin(volR, 32000.0f);
	s_rn.vol[1] = (int32_t)rnMin(volL, 32000.0f);
	// RoadNoiseSpeedToPitch (0x803A52AC).
	const float p = slopeY(1500.0f, 4500.0f, 0.0f, 100.0f, s_rn.mph);
	float pL = p * (1.0f + rnMin(lS * 0.01f, 0.2f));
	pL *= 1.0f + rnMin((gr[0] + gr[3]) * 0.5f * 0.15f, 0.15f);
	float pR = p * (1.0f + rnMin(rS * 0.01f, 0.2f));
	pR *= 1.0f + rnMin((gr[1] + gr[2]) * 0.5f * 0.15f, 0.15f);
	s_rn.pitch[0] = (int32_t)rnMin(pR, 6000.0f);
	s_rn.pitch[1] = (int32_t)rnMin(pL, 6000.0f);
}

// CARSFX_RoadNoise::GenTerrainType (0x8009D548): each side's terrain off
// SFXCTL_Wheel, and whether it changed. Both start at 0, so the first frame
// is a change.
static void rnGenTerrain(void)
{
	const int t[2] = { s_sk.terrain[1], s_sk.terrain[0] };
	for (int s = 0; s < 2; s++) {
		s_rn.changed[s] = t[s] != s_rn.terrain[s];
		s_rn.oldTerrain[s] = s_rn.terrain[s];
		s_rn.terrain[s] = t[s];
	}
}

// CARSFX_RoadNoise::Play (0x8009D5D4): the side's loop, anew.
static void rnPlay(int loop, int left)
{
	if (!s_rn.enabled) return;
	letGo(&s_rn.loop[left]);
	int32_t* d = s_rn.loopData[left];
	memset(d, 0, sizeof(s_rn.loopData[left]));
	d[0] = clampi(loop, 0, 15);
	d[2] = 0x1000;
	s_rn.loop[left] = aemsCreate(aemsClass("FX_ROADNOISE"), d);
}

// CARSFX_RoadNoise::PlayTransition (0x8009CB30): the side's one-shot, at
// RoadNoiseTransitionVolSlope's level (0x803A524C) and
// RoadNoiseTransitionPitchSlope's pitch (0x803A5268). Then, if the side has
// no stitch playing, the next of 0x4E..0x51 by its count. Its SND_Params are
// all zero: Play's 0x7FFF and 0x1000 stores land on the same words as the
// zeros after them, so it starts silent and takes its level from
// ProcessUpdate's first Update. A stitch that is not there (port-only guard;
// retail news it regardless) leaves the side free for the next.
static void rnPlayTransition(int id, int left)
{
	if (!s_rn.enabled) return;
	int32_t vol = (int32_t)slopeY(20000.0f, 32767.0f, 12.0f, 60.0f, s_rn.mph) * rnTransitionVol(id) >> 15;
	const int32_t pitch = (int32_t)slopeY(3600.0f, 4400.0f, 12.0f, 60.0f, s_rn.mph);
	vol = vol * rnQ15(left ? 3 : 2) >> 15;
	letGo(&s_rn.trans[left]);
	int32_t* d = s_rn.transData[left];
	memset(d, 0, sizeof(s_rn.transData[left]));
	d[0] = clampi(id, 0, 15);
	d[1] = clampi(vol, 0, 0x7fff);
	d[2] = clampi(pitch, 0, 0x1fff);
	d[3] = clampi(rnAzim(left ? 3 : 2), 0, 0xffff);
	d[4] = 1;
	s_rn.trans[left] = aemsCreate(aemsClass("FX_ROADNOISE_TRANS"), d);
	int idx = -1;
	if (!s_rn.stitch[left]) {
		static const int32_t kZero[5] = { 0 };
		int* n = &s_rnStitchCount[left];
		*n %= 4;
		idx = 0x4E + *n;
		(*n)++;
		s_rn.stitch[left] = colsfxStitchPlay(0, idx, kZero);
	}
	if (s_rnTransLogs < 40) {
		s_rnTransLogs++;
		sfxLog("road transition: %s %d -> %d, id %d vol %d pitch %d q15 %d; stitch %d %s, q15 %d",
		       left ? "L" : "R", s_rn.oldTerrain[left], s_rn.terrain[left], id, (int)d[1],
		       (int)d[2], (int)rnQ15(left ? 3 : 2), idx,
		       idx < 0 ? "(side busy)" : s_rn.stitch[left] ? "playing" : "none",
		       (int)rnQ15(left ? 7 : 6));
	}
}

// ProcessUpdate's stitch half for one side (+0xD4 left, +0xD0 right): gone
// once it has stopped playing, else updated at its mix map output's level
// and azimuth (7 left, 6 right) and pitch 0, which the samples ignore: they
// keep the pitch they started with.
static void rnStitchUpdate(int left)
{
	ColsfxStitch** w = &s_rn.stitch[left];
	if (!*w) return;
	if (!colsfxStitchPlaying(*w)) {
		colsfxStitchFree(*w);
		*w = NULL;
		return;
	}
	const int out = left ? 7 : 6;
	const int32_t p[5] = { 0, rnQ15(out), 0, rnAzim(out), 0 };
	colsfxStitchUpdate(*w, p);
}

// The terrain-change half of ProcessUpdate for one side: its new loop (or
// none), then, with a wheel down, the transition off the old terrain or else
// on to the new one.
static void rnTerrainChange(int left)
{
	if (!s_rn.changed[left]) return;
	s_rn.loopId[left] = rnLoopId(s_rn.terrain[left]);
	const int onTo = rnOnTo(s_rn.terrain[left]);
	const int offOf = rnOffOf(s_rn.oldTerrain[left]);
	if (s_rn.loopId[left] != -1)
		rnPlay(s_rn.loopId[left], left);
	else
		letGo(&s_rn.loop[left]);
	if (!(left ? s_sk.lDown : s_sk.rDown)) return;
	if (offOf != -1)
		rnPlayTransition(offOf, left);
	else if (onTo != -1)
		rnPlayTransition(onTo, left);
}

static void rnSet(AemsInst inst, int32_t* d, int32_t vol, int32_t pitch, int32_t azim)
{
	if (!inst) return;
	d[1] = clampi(vol, 0, 0x7fff);
	d[2] = clampi(pitch, 0, 0x1fff);
	d[3] = clampi(azim, 0, 0xffff);
	aemsSetMemberData(inst, d);
}

// While paused, a transition still sounding (refcount > 1) goes silent at
// the pause's pitch.
static void rnTransPause(int left)
{
	AemsInst t = s_rn.trans[left];
	if (!t || aemsRefCount(t) <= 1 || !s_paused) return;
	int32_t pitch = 0x1000;
	adjustPitch(&pitch);
	int32_t* d = s_rn.transData[left];
	d[1] = 0;
	d[2] = clampi(pitch, 0, 0x1fff);
	aemsSetMemberData(t, d);
}

// CARSFX_RoadNoise::ProcessUpdate (0x8009C224). Retail's quirks, kept: the
// right loop's pitch is not pause-adjusted, the right rumble takes the left
// side's level, and the dirt flag ([5]) is only ever set.
static void rnProcessUpdate(void)
{
	if (!s_rn.enabled) return;
	rnTerrainChange(1);
	rnTerrainChange(0);
	const int32_t vol4 = kRoadNoiseVolumes[4];
	const int idL = s_rn.loopId[1], idR = s_rn.loopId[0];
	const int32_t volIdL = idL >= 0 && idL < 8 ? kRoadNoiseVolumes[idL] : 0;
	const int32_t volIdR = idR >= 0 && idR < 8 ? kRoadNoiseVolumes[idR] : 0;
	// The left loop and rumble.
	int32_t v = (s_rn.vol[1] * volIdL >> 15) * rnQ15(1) >> 15;
	int32_t pitchL = s_rn.pitch[1];
	adjustPitch(&pitchL);
	if (!s_sk.lDown) v = 0;
	rnSet(s_rn.loop[1], s_rn.loopData[1], v, pitchL, rnAzim(1));
	rnSet(s_rn.rumble[1], s_rn.rumbleData[1], (s_rn.vol[1] * vol4 >> 15) * rnQ15(5) >> 15, pitchL, rnAzim(5));
	rnTransPause(1);
	rnStitchUpdate(1);
	// The right loop and rumble.
	v = (s_rn.vol[0] * volIdR >> 15) * rnQ15(0) >> 15;
	if (!s_sk.rDown) v = 0;
	rnSet(s_rn.loop[0], s_rn.loopData[0], v, s_rn.pitch[0], rnAzim(0));
	rnSet(s_rn.rumble[0], s_rn.rumbleData[0], (s_rn.vol[1] * vol4 >> 15) * rnQ15(4) >> 15, s_rn.pitch[0], rnAzim(4));
	rnTransPause(0);
	rnStitchUpdate(0);
	for (int s = 1; s >= 0; s--)
		if ((unsigned)(s_rn.terrain[s] - 8) <= 1 && s_rn.loop[s])
			s_rn.loopData[s][5] = 1;
}

// CARSFX_WindNoise::UpdateParams (0x8009DD10): the speed, 2..40 m/s (past 40
// it reads 39.95), as a fraction of 40.
static void wnUpdateParams(const CarSfxIn* in)
{
	if (!s_wn.enabled) return;
	float s = in->speed;
	if (s < 2.0f) s = 2.0f;
	else if (s > 40.0f) s = 39.95f;
	const float f = s / 40.0f;
	s_wn.f198 = (int32_t)(f * 1040.0f);
	s_wn.vol = (int32_t)(f * 32767.0f);
}

// CARSFX_WindNoise::UpdateCSISParams (0x8009E2A4), under gfWindMasterVolume
// (0x7fff).
static void wnUpdateCsis(void)
{
	if (!s_wn.enabled || !s_wn.wind) return;
	int32_t* d = s_wn.data;
	int32_t pitch = 0x1000;
	adjustPitch(&pitch);
	d[1] = clampi((wnQ15(0) * s_wn.vol >> 15) * 0x7fff >> 15, 0, 0x7fff);
	d[4] = clampi((wnQ15(1) * s_wn.vol >> 15) * 0x7fff >> 15, 0, 0x7fff);
	d[2] = clampi(pitch, 0, 0x1fff);
	d[3] = clampi(wnAzim(0), 0, 0xffff);
	d[5] = clampi(wnAzim(1), 0, 0xffff);
	d[6] = clampi(s_wn.f198, 0, 0x410);
	aemsSetMemberData(s_wn.wind, d);
}

// CARSFX_WindWeather::UpdateParams (0x8009E664), outside the race
// coordinator's Loading state (RCState 1; the port runs no SFX then):
// WeatherWindVolSlope (0x803A52C8), Slope(1, 0.25, 0, 70) built at
// 0x800B91F0, of the speed in mph (Car +0x440 x 2.237) -- the whole wind at a
// standstill, a quarter of it from 70 mph.
static void wwUpdateParams(const CarSfxIn* in)
{
	if (!s_ww.inst) return;
	s_ww.vol = slopeY(1.0f, 0.25f, 0.0f, 70.0f, in->speed * 2.237f);
}

// CARSFX_WindWeather::ProcessUpdate (0x8009E7DC): [0] the level, [2] 4000;
// [1] stays at Play's 0.
static void wwProcessUpdate(void)
{
	if (!s_ww.inst) return;
	s_ww.data[0] = clampi((int32_t)((float)wwQ15(0) * s_ww.vol), 0, 0x7fff);
	s_ww.data[2] = 4000;
	s_ww.data[3] = 0;
	aemsSetMemberData(s_ww.inst, s_ww.data);
}

// CARSFX_RoadNoise's InitSFX (0x8009BD8C) and SetupLoadData (0x800AEB50): a
// rumble a side, sample 4, its loops waiting on the first terrain; and
// CARSFX_WindNoise's InitSFX (0x8009DBAC), its one FX_WIND.
static void roadWindSetup(const char* audioDir, AudioBus roadBus, AudioBus windBus)
{
	char path[512];
	memset(&s_rn, 0, sizeof(s_rn));
	memset(&s_wn, 0, sizeof(s_wn));
	memset(&s_ww, 0, sizeof(s_ww));
	s_rnTransLogs = 0;
	snprintf(path, sizeof(path), "%sIG_GLOBAL/ROADNOISE_00_MB.aem", audioDir);
	s_rn.bank = aemsLoad(path, roadBus);
	if (s_rn.bank) {
		s_rn.enabled = true;
		const AemsClass cls = aemsClass("FX_ROADNOISE");
		for (int s = 0; s < 2; s++) {
			s_rn.rumbleData[s][0] = 4;
			s_rn.rumble[s] = aemsCreate(cls, s_rn.rumbleData[s]);
		}
	} else {
		sfxLog("road noise: no %s -- the road silent (tools/abk3ds.py)", path);
	}
	snprintf(path, sizeof(path), "%sIG_GLOBAL/WIND_00_MB.aem", audioDir);
	s_wn.bank = aemsLoad(path, windBus);
	if (s_wn.bank) {
		s_wn.enabled = true;
		s_wn.data[2] = 0x1000;
		s_wn.wind = aemsCreate(aemsClass("FX_WIND"), s_wn.data);
		// CARSFX_WindWeather::Play (0x8009E6EC), which retail calls from its
		// first UpdateParams; the port's SFX first run in the world, so here.
		s_ww.inst = aemsCreate(aemsClass("FX_WIND_Weather"), s_ww.data);
	} else {
		sfxLog("wind: no %s -- the wind silent (tools/abk3ds.py)", path);
	}
	sfxLog("road noise: rumbles %s/%s; wind: %s; weather wind: %s", s_rn.rumble[1] ? "ok" : "none",
	       s_rn.rumble[0] ? "ok" : "none", s_wn.wind ? "ok" : "none", s_ww.inst ? "ok" : "none");
}

static void roadWindShutdown(void)
{
	for (int s = 0; s < 2; s++) {
		letGo(&s_rn.rumble[s]);
		letGo(&s_rn.loop[s]);
		letGo(&s_rn.trans[s]);
		colsfxStitchFree(s_rn.stitch[s]);
	}
	letGo(&s_wn.wind);
	letGo(&s_ww.inst);
	if (s_rn.bank) aemsUnload(s_rn.bank);
	if (s_wn.bank) aemsUnload(s_wn.bank);
	memset(&s_rn, 0, sizeof(s_rn));
	memset(&s_wn, 0, sizeof(s_wn));
	memset(&s_ww, 0, sizeof(s_ww));
}

// Debug: every 5 s, what the road and the wind are playing.
static void roadWindReport(float dt)
{
	if (!s_rn.enabled && !s_wn.enabled) return;
	if ((s_rwReportIn -= dt) > 0.0f) return;
	s_rwReportIn = 5.0f;
	sfxLog("road: %.1f mph, terrain L %d R %d, loop L %d R %d; level L %d R %d, pitch L %d R %d; "
	       "loop vol L %d R %d, rumble vol L %d R %d; q15 %d/%d/%d/%d az %04x/%04x",
	       s_rn.mph, s_rn.terrain[1], s_rn.terrain[0], s_rn.loopId[1], s_rn.loopId[0],
	       (int)s_rn.vol[1], (int)s_rn.vol[0], (int)s_rn.pitch[1], (int)s_rn.pitch[0],
	       (int)s_rn.loopData[1][1], (int)s_rn.loopData[0][1],
	       (int)s_rn.rumbleData[1][1], (int)s_rn.rumbleData[0][1],
	       (int)rnQ15(1), (int)rnQ15(0), (int)rnQ15(5), (int)rnQ15(4),
	       (unsigned)rnAzim(1), (unsigned)rnAzim(0));
	sfxLog("wind: vol %d f198 %d; member vol %d/%d az %04x/%04x [6] %d; weather x%.2f q15 %d -> %d",
	       (int)s_wn.vol, (int)s_wn.f198, (int)s_wn.data[1], (int)s_wn.data[4],
	       (unsigned)s_wn.data[3], (unsigned)s_wn.data[5], (int)s_wn.data[6],
	       s_ww.vol, (int)wwQ15(0), (int)s_ww.data[0]);
}

// ---------------------------------------------------------------- rain

// CARSFX_Rain (Speed.elf 0x800A1668..0x800A1CF0, SetupLoadData 0x800AF98C;
// tools/re/wip315/carsfx_rain_ps.c), the PlayerCar state's SFX 9 (TASKS.md
// #315): the rain on the car, one Csis FX_Weather out of IG_GLOBAL/FX_Rain_MB
// (data group 11), and the thunder, a NIS stream (speech.h).
//
//   UpdateParams  the view's Rain intensity (+0x1F0) over 0.001 plays it, at
//                 or under it stops it: member [5] set, a 10 s fade, then the
//                 instance goes. Over 0.25, with no world data streaming, a
//                 timer asks for thunder: at once the first time, then
//                 bRandom(20) + 25 s on -- and after each wait a counter
//                 runs 32 frames past it before the next ask.
//   QueueWeatherStream  QueStream(4, "THUNDERSTRM", intensity < 0.5 ? 1 : 2)
//                 while the NIS module is done playing, the race coordinator
//                 is not in states 2, 3 or 7 and the game mode is not 8.
//   ProcessUpdate members {inside the car (hood or bumper camera), 0, its
//                 Q15 out of the map's output 1 inside or 0 outside, 7000,
//                 0}; [5] is Stop's.
//   UpdateMixerOutputs  output +0xC is 1 while it plays and is not stopping.
//
// PORT: QueStream's own gate (r13 - 0x78EC, set by the NIS player) is not
// ported, and IsStrmQueued is always false (the port's NIS stream plays at
// once). Game mode 8 has no counterpart in the port.
typedef struct {
	AemsBank* bank;          // IG_GLOBAL/FX_Rain_MB
	AemsInst  inst;          // +0xD0: its Csis instance
	int32_t   data[6];       // +0xD0 + 4: the member data
	float     rain, oldRain; // +0xB0 / +0xB4: the view's intensity, now and last frame
	float     t, next;       // +0xB8 / +0xBC: the thunder timer and its wait
	int       queued;        // +0xC0: a thunder stream asked for
	int       count;         // +0xC4: the 32-frame counter
	float     fade;          // +0xC8: Stop's fade, s
	int       stopping;      // +0xCC
	int       thunders;      // debug: asks made
} RainSfx;

static RainSfx s_rs;

static void rsSetup(const char* audioDir, AudioBus bus)
{
	char path[512];
	memset(&s_rs, 0, sizeof(s_rs));
	snprintf(path, sizeof(path), "%sIG_GLOBAL/FX_Rain_MB.aem", audioDir);
	s_rs.bank = aemsLoad(path, bus);
	sfxLog("rain sound: %s", s_rs.bank ? "FX_Rain_MB loaded" : "no FX_Rain_MB -- the rain silent (tools/abk3ds.py)");
}

// Play (0x800A1764): a new FX_Weather, its members zeroed.
static void rsPlay(void)
{
	if (s_rs.inst) {
		letGo(&s_rs.inst);
		s_rs.stopping = 0;
		return;
	}
	if (!s_rs.bank) return;
	memset(s_rs.data, 0, sizeof(s_rs.data));
	s_rs.inst = aemsCreate(aemsClass("FX_Weather"), s_rs.data);
	s_rs.stopping = 0;
	sfxLog("rain sound: play at intensity %.2f%s", s_rs.rain, s_rs.inst ? "" : " -- FX_Weather not created");
}

// Stop (0x800A186C).
static void rsStop(void)
{
	if (!s_rs.inst) return;
	s_rs.stopping = 1;
	s_rs.fade = 10.0f;
	s_rs.data[5] = 1;
	aemsSetMemberData(s_rs.inst, s_rs.data);
	sfxLog("rain sound: stop, a 10 s fade");
}

// QueueWeatherStream (0x800A1948).
static void rsQueueWeatherStream(const CarSfxIn* in)
{
	if (s_rs.queued == 1 || !in->weatherStreamOk) return;
	const int variant = s_rs.rain < 0.5f ? 1 : 2;
	if (speechNisPlaying()) {
		sfxLog("thunder: the NIS module is busy -- no ask");
		return;
	}
	s_rs.thunders++;
	sfxLog("thunder: QueStream THUNDERSTRM %d at intensity %.2f (next in %.0f s)", variant, s_rs.rain,
	       s_rs.next);
	speechThunderStream(variant, s_rng);
	s_rs.queued = 1;
}

// UpdateParams (0x800A1A48).
static void rsUpdateParams(const CarSfxIn* in, float dt)
{
	s_rs.oldRain = s_rs.rain;
	s_rs.rain = in->rain;
	if (s_rs.rain <= 0.001f) {
		if (s_rs.inst && !s_rs.stopping) rsStop();
	} else if (!s_rs.inst) {
		rsPlay();
	}
	s_rs.t += dt;
	if (s_rs.rain > 0.25f && !in->worldStreaming && s_rs.next < s_rs.t) {
		if (s_rs.count == 0) {
			s_rs.t = 0.0f;
			// !IsStrmQueued
			if (s_rs.queued == 1) s_rs.queued = 0;
			// Retail asks, then draws the wait. The port draws first: its
			// alternate's pick also comes out of bRandom (SPCH has its own
			// random), so this keeps retail's draw ahead of the port's
			// extra one, and the log reads the new wait.
			const float wait = (float)rideinfo_random(s_rng, 20) + 25.0f;
			s_rs.next = wait;
			rsQueueWeatherStream(in);
			s_rs.count++;
		} else if (++s_rs.count > 0x20) {
			s_rs.count = 0;
		}
	}
	if (s_rs.stopping) {
		s_rs.fade -= dt;
		if (s_rs.fade < 0.0f) {
			letGo(&s_rs.inst);
			s_rs.stopping = 0;
		}
	}
}

// UpdateMixerOutputs (0x800A1C58).
static void rsUpdateMixerOutputs(void)
{
	int32_t* o = mixmapObjOut(MIXMAP_SFX_RAIN);
	if (o) o[2] = s_rs.inst && !s_rs.stopping;
}

// ProcessUpdate (0x800A1C88).
static void rsProcessUpdate(const CarSfxIn* in)
{
	if (!s_rs.inst) return;
	const bool inside = in->eng.povType == CARDB_POV_HOOD || in->eng.povType == CARDB_POV_BUMPER;
	s_rs.data[2] = clampi(mixmapQ15(MIXMAP_SFX_RAIN, inside ? 1 : 0), 0, 0x7fff);
	s_rs.data[4] = 0;
	s_rs.data[0] = inside;
	s_rs.data[1] = 0;
	s_rs.data[3] = 7000;
	aemsSetMemberData(s_rs.inst, s_rs.data);
}

static void rsShutdown(void)
{
	letGo(&s_rs.inst);
	if (s_rs.bank) aemsUnload(s_rs.bank);
	memset(&s_rs, 0, sizeof(s_rs));
}

// ---------------------------------------------------------------- landings

// CARSFX_BottomOut (Speed.elf 0x800A5998..0x800A6530), the PlayerCar state's
// SFX 11: the thump of a jump's landing and of the floor striking the road,
// both stitches out of MB_Stich_Collision (colsfx.c). Its outputs +4 and +8
// (a landing, a bottom-out this frame) start the map's envelopes 1 and 2.
//
// The landings watch the wheels in pairs (Car +0x388 [i]): the front
// (0, 1), the rear (2, 3) and the two sides (1, 2) and (3, 0).
// Retail's air timer is set to the state's clock (CSTATE +0x34) when the pair
// lifts and then counts on from there, so its "in the air over 0.05 s" test
// holds for nearly any lift; ported as it is.
static const int kBoPair[4][2] = { { 0, 1 }, { 2, 3 }, { 1, 2 }, { 3, 0 } };

typedef struct {
	ColsfxStitch* bottom;   // +0xB0, the bottom-out
	ColsfxStitch* land;     // +0xB4, the landing
	ColsfxStitch* jumpCrash; // +0xB8, m_pJumpCamCrash: a landing under zone 6's camera
	bool  wasDown[4];       // +0xBC / +0xC4 / +0xCC / +0xD4, by kBoPair
	float air[4];           // +0xC0 / +0xC8 / +0xD0 / +0xD8
	bool  heavy;            // +0xDC: leaning hard with a wheel up
	float clock;            // CSTATE +0x34: the state's unpaused time
} BottomOut;

static BottomOut s_bo;
// The LastRandom statics: each stitch row's next of its four takes.
static int s_boLandN, s_boHeavyN, s_boBottomN, s_boCrashN;   // r13 - 0x7738 / 0x773C / 0x7730 / 0x7734
static int s_boLogs;   // debug: the first few plays

static int32_t boQ15(int output)  { return mixmapQ15(MIXMAP_SFX_BOTTOMOUT, output); }
static int32_t boAzim(int output) { return mixmapAzim(MIXMAP_SFX_BOTTOMOUT, output); }

static void boReset(void)
{
	colsfxStitchFree(s_bo.bottom);
	colsfxStitchFree(s_bo.land);
	colsfxStitchFree(s_bo.jumpCrash);
	memset(&s_bo, 0, sizeof(s_bo));
	// The constructor: the front and rear pairs start down.
	s_bo.wasDown[0] = s_bo.wasDown[1] = true;
	int32_t* o = mixmapObjOut(MIXMAP_SFX_BOTTOMOUT);
	if (o) o[0] = o[1] = 0;
}

// The next stitch of a row of four: 0x4E.. or 0x56.. plus 4 a step up.
static int boNext(int* n, int base, int step)
{
	*n %= 4;
	return base + (*n)++ + 4 * step;
}

// LandJumpPlay (0x800A5B84): the landing, `p` 0..127. Light, stitch 0x4E..0x55
// by 2p / 128; heavy, 0x56..0x59. Played at Play(0, 0, 0), silent until
// ProcessUpdate's first Update gives it its level. With the car's nearest
// player (GetClosestPlayer, the port's one) in zone 6 -- the crash camera
// (main.c, TASKS.md #335) -- a crash stitch too, in place of the last one:
// 0x2E..0x31 or 0x32..0x35 by bRandom(127) x 2 / 128.
static void boLandPlay(float p, bool heavy, int zone)
{
	static const int32_t kZero[5] = { 0 };
	colsfxStitchFree(s_bo.land);
	s_bo.land = NULL;
	if (p < 0.0f) p = 0.0f;
	if (p > 127.0f) p = 127.0f;
	const int idx = heavy ? boNext(&s_boHeavyN, 0x56, (int)(p * (1.0f / 128.0f)))
		: boNext(&s_boLandN, 0x4E, (int)(p * (1.0f / 128.0f) + p * (1.0f / 128.0f)));
	s_bo.land = colsfxStitchPlay(0, idx, kZero);
	int crash = -1;
	if (zone == 6) {
		colsfxStitchFree(s_bo.jumpCrash);
		const float r = difficulty_random(s_rng, 127.0f);
		crash = boNext(&s_boCrashN, 0x2E, (int)(r * (1.0f / 128.0f) + r * (1.0f / 128.0f)));
		s_bo.jumpCrash = colsfxStitchPlay(0, crash, kZero);
	}
	int32_t* o = mixmapObjOut(MIXMAP_SFX_BOTTOMOUT);
	if (o) o[0] = 1;
	if (s_boLogs < 30 || crash >= 0) {
		s_boLogs++;
		sfxLog("landing: %s, intensity %.0f -> stitch 0x%X %s", heavy ? "heavy" : "light", p, idx,
		       s_bo.land ? "playing" : "(none)");
		if (crash >= 0)
			sfxLog("  zone 6: crash stitch 0x%X %s", crash, s_bo.jumpCrash ? "playing" : "(none)");
	}
}

// BottomOutPlay (0x800A5E10): the floor struck at `i` 0..127, stitch 0x4E +
// 4 * (i / 32) on, unless the last one is still playing.
static void boBottomPlay(int i)
{
	static const int32_t kZero[5] = { 0 };
	if (s_bo.bottom) return;
	const float f = i < 0 ? 0.0f : i > 127 ? 127.0f : (float)i;
	const int idx = boNext(&s_boBottomN, 0x4E, (int)(f * (1.0f / 128.0f) * 4.0f));
	s_bo.bottom = colsfxStitchPlay(0, idx, kZero);
	int32_t* o = mixmapObjOut(MIXMAP_SFX_BOTTOMOUT);
	if (o) o[1] = 1;
	if (s_boLogs < 30) {
		s_boLogs++;
		sfxLog("bottom out: intensity %d -> stitch 0x%X %s", i, idx, s_bo.bottom ? "playing" : "(none)");
	}
}

// UpdateParams (0x800A5FE8), before the map reads +4 / +8.
static void boUpdateParams(const CarSfxIn* in, float dt)
{
	// CSTATE_Base::UpdateParams (0x800AC158) runs the state's clock first.
	if (!in->paused) s_bo.clock += dt;
	bool pair[4];
	int down = 0;
	for (int k = 0; k < 4; k++) {
		pair[k] = in->wheel[kBoPair[k][0]].onGround && in->wheel[kBoPair[k][1]].onGround;
		down += in->wheel[k].onGround;
	}
	// The bottom-out, from Car::TriggerCarTerrainEffects through
	// EAXTunerCar::SFXMessage 5, for the human driver only.
	if (in->eng.human && in->bottomOut >= 0) boBottomPlay(in->bottomOut);
	// DOT_PROD_FOR_HEAVY_LEAN (0.9, r13 - 0x7740) against the car's up axis,
	// with a wheel off (GetWheelsOnGround < 1).
	if (!s_bo.heavy && in->upZ < 0.9f && down < 4) s_bo.heavy = true;
	bool play = false, heavy = false;
	if (s_bo.heavy) {
		// Leaning hard: any pair coming down.
		for (int k = 0; k < 4; k++)
			if (pair[k] && !s_bo.wasDown[k]) play = heavy = true;
	} else {
		// Level: the front or the rear pair, after 0.05 s up.
		for (int k = 0; k < 2; k++)
			if (pair[k] && s_bo.air[k] > 0.05f && !s_bo.wasDown[k]) play = true;
	}
	if (play) {
		// On the PhysicsMover (Car +0x28 == 1), how far each spring is into
		// its bump stop, through JumpLandingIntensity (Slope 0.4..1 over
		// 0.4..1, built at 0x800B9318); on any other mover, the loudest.
		float p = 127.0f;
		if (in->physicsMover) {
			float sum = 0.0f;
			for (int i = 0; i < 4; i++) {
				const float r = fabsf(in->suspRatio[i]);
				sum += r < 0.4f ? 0.4f : r > 1.0f ? 1.0f : r;
			}
			p = sum * 0.25f * 127.0f;
		}
		boLandPlay(p, heavy, in->eng.zone);
	}
	if (down == 4 && s_bo.heavy) s_bo.heavy = false;
	for (int k = 0; k < 4; k++) {
		if (pair[k]) {
			s_bo.wasDown[k] = true;
			continue;
		}
		if (s_bo.wasDown[k]) {
			s_bo.wasDown[k] = false;
			s_bo.air[k] = s_bo.clock;
		}
		s_bo.air[k] += dt;
	}
}

// One stitch's ProcessUpdate: its level and azimuth off the map's `output`
// (0, the zone 6 crash 2), pitch 0x1000 through AdjustPitch; gone once it has
// stopped (and the landing under the pause too).
static void boStitchUpdate(ColsfxStitch** w, int output, bool freeIfPaused)
{
	if (!*w) return;
	int32_t pitch = 0x1000;
	adjustPitch(&pitch);
	const int32_t p[5] = { 0, boQ15(output), pitch, boAzim(output), 0 };
	colsfxStitchUpdate(*w, p);
	if (!colsfxStitchPlaying(*w) || (freeIfPaused && s_paused)) {
		colsfxStitchFree(*w);
		*w = NULL;
	}
}

// ProcessUpdate (0x800A6530), after the map: +4 and +8 back to 0, then the
// stitches.
static void boProcessUpdate(void)
{
	int32_t* o = mixmapObjOut(MIXMAP_SFX_BOTTOMOUT);
	// Debug: the level the map gave a thump the frame it started.
	if (o && (o[0] || o[1]) && s_boLogs < 30) {
		s_boLogs++;
		sfxLog("  its mix: q15 %d azim %04x", (int)boQ15(0), (unsigned)boAzim(0));
	}
	if (o) o[0] = o[1] = 0;
	boStitchUpdate(&s_bo.bottom, 0, false);
	boStitchUpdate(&s_bo.jumpCrash, 2, false);
	boStitchUpdate(&s_bo.land, 0, true);
}

// ---------------------------------------------------------------- tunnels

// SFXCTL_Tunnel (Speed.elf 0x80095A58..0x80096744), the PlayerCar state's
// SFXCTL 7: whether the car is under a roof, by the TrackPathZones it is in
// (TPZ_TUNNEL, TPZ_OVERPASS, TPZ_OVERPASS_SMALL, TPZ_GARAGE), and, 0.3 s down
// its heading, whether it is about to go in or come out. Its outputs move
// the map's road noise up in a tunnel and duck the music in a garage; its
// enter and exit flags start CARSFX_DriveBy's wooshes, placed where the
// zone was found (SFXCTL_3DTunnelPos, SFXCTL 10).
//
// Not ported: the reverb half -- SetCurrentReverbType, EndTunnelVerb,
// UpdateCityVerb (the ReverbAccessor's zones) and UpdateReflectionParams --
// which sets the EAX / DSP reverb's levels. NDSP has no reverb unit to set.
typedef struct {
	bool  in;           // +0x70: in a zone (the reverb's switch)
	int   type;         // +0x7C: which, 5 tunnel, 6 / 7 overpass, 20 garage
	int   aheadType;    // +0x78: the zone 0.3 s ahead, 5 / 6 / 7, or 2 none
	bool  enter, exit;  // +0x80 / +0xA0: a woosh to start this frame
	float point[3];     // +0x84: where (3DTunnelPos's object)
	float enterVol;     // +0x94: WooshVol_vs_Vel then
	float enterWait;    // +0x98: no second woosh at the same zone for 3 s
	const TrackPathZone* enterZone;   // +0x9C
	float exitVol;      // +0xA4
	float exitWait;     // +0xA8
	const TrackPathZone* exitZone;    // +0xAC
} Tunnel;

static Tunnel s_tn;
static int s_tnLogs;   // debug: the first few zones and wooshes

// The first zone of `type` at `xy` whose roof (TrackPathZone +0x14) is unset
// or not under `z`: FindZone's loop as UpdateDriveBySnds and UpdateParams
// run it.
static const TrackPathZone* tnFind(const TrackPaths* tp, const float xy[2], float z, u32 type)
{
	if (!tp) return NULL;
	const TrackPathZone* zn = NULL;
	while ((zn = trackpath_find(tp, xy[0], xy[1], type, zn)) != NULL)
		if (zn->entryZ == 0.0f || !(zn->entryZ < z)) return zn;
	return NULL;
}

// g_WooshVol_vs_Vel (0x803A5414), Slope(0, 1, 18, 45) built at 0x800B92E4:
// 0 at 18 m/s to 1 at 45.
static float tnWooshVol(float speed) { return slopeY(0.0f, 1.0f, 18.0f, 45.0f, speed); }

// UpdateDriveBySnds (0x80095D3C), on last frame's zone: outside one, a zone
// ahead raises the enter woosh; in a tunnel (not under an overpass), open
// road ahead raises the exit woosh. Each waits 3 s before it will sound
// again for the same zone, and neither sounds under 18 m/s.
static void tnDriveBy(const CarSfxIn* in, float dt)
{
	s_tn.exit = s_tn.enter = false;
	s_tn.enterWait = fmaxf(s_tn.enterWait - dt, 0.0f);
	s_tn.exitWait = fmaxf(s_tn.exitWait - dt, 0.0f);
	s_tn.aheadType = 2;
	// bNormalize of the heading's XY (Car +0x88), times the forward speed
	// (CarState +0x3FC) and 0.3 s.
	float d[2] = { in->fwd[0], in->fwd[1] };
	const float len = sqrtf(d[0] * d[0] + d[1] * d[1]);
	if (len > 0.0f) { d[0] /= len; d[1] /= len; } else { d[0] = 1.0f; d[1] = 0.0f; }
	const float k = in->vfwd * 0.3f;
	const float ahead[2] = { in->pos[0] + d[0] * k, in->pos[1] + d[1] * k };
	const float z = in->pos[2];
	const TrackPathZone* zn;
	if ((zn = tnFind(in->paths, ahead, z, TPZ_TUNNEL)) != NULL) s_tn.aheadType = 5;
	else if ((zn = tnFind(in->paths, ahead, z, TPZ_OVERPASS)) != NULL) s_tn.aheadType = 6;
	else if ((zn = tnFind(in->paths, ahead, z, TPZ_OVERPASS_SMALL)) != NULL) s_tn.aheadType = 7;
	const float vol = tnWooshVol(in->speed);
	if (!s_tn.in) {
		if (s_tn.aheadType != 2 &&
		    ((s_tn.enterWait < 0.01f && s_tn.enterZone == zn) || s_tn.enterZone != zn) && vol > 0.01f) {
			s_tn.enter = true;
			s_tn.enterWait = 3.0f;
			s_tn.enterZone = zn;
			s_tn.point[0] = ahead[0];
			s_tn.point[1] = ahead[1];
			s_tn.point[2] = z + 10.0f;
			s_tn.enterVol = vol;
		}
		return;
	}
	if (s_tn.aheadType == 2 && s_tn.type == 5 &&
	    ((s_tn.exitWait < 0.01f && s_tn.exitZone == zn) || s_tn.exitZone != zn) && vol > 0.01f) {
		s_tn.exit = true;
		s_tn.exitWait = 3.0f;
		s_tn.exitZone = zn;
		s_tn.point[0] = ahead[0];
		s_tn.point[1] = ahead[1];
		s_tn.point[2] = z + 10.0f;
		s_tn.exitVol = vol;
	}
}

// UpdateParams (0x80096114) and UpdateMixerOutputs (0x800963D0): the
// drive-bys, then which zone the car itself is in. Outputs +4 a tunnel, +8
// an overpass, +0xC a garage, +0x10 any.
static void tnUpdate(const CarSfxIn* in, float dt)
{
	tnDriveBy(in, dt);
	const float xy[2] = { in->pos[0], in->pos[1] };
	bool found = true;
	if (tnFind(in->paths, xy, in->pos[2], TPZ_TUNNEL)) s_tn.type = 5;
	else if (tnFind(in->paths, xy, in->pos[2], TPZ_OVERPASS)) s_tn.type = 6;
	else if (tnFind(in->paths, xy, in->pos[2], TPZ_OVERPASS_SMALL)) s_tn.type = 7;
	else if (tnFind(in->paths, xy, in->pos[2], TPZ_GARAGE)) s_tn.type = 20;
	else found = false;
	if (found != s_tn.in) {
		s_tn.in = found;
		if (s_tnLogs < 40) {
			s_tnLogs++;
			if (found) sfxLog("tunnel: in, zone type %d", s_tn.type);
			else sfxLog("tunnel: out");
		}
	}
	int32_t* o = mixmapObjOut(MIXMAP_CTL_TUNNEL);
	if (o) {
		o[0] = o[1] = o[2] = o[3] = 0;
		if (s_tn.in) {
			if (s_tn.type == 6 || s_tn.type == 7) o[1] = 1;
			else if (s_tn.type == 20) o[2] = 1;
			else o[0] = 1;
			o[3] = 1;
		}
	}
	mixmapSetObjPos(MIXMAP_CTL_3DTUNNELPOS, s_tn.point);
}

// CARSFX_DriveBy (0x800A1DD0..0x800A23B0), the PlayerCar state's SFX 12: the
// woosh of a tunnel or overpass going past, MB_Stitch_Whoosh's stitches
// (colsfx.c type 1), two at a time each way. Each one's SND_Params [0] is
// the map output it reads: 0 going in, 4 coming out. Its output +0x18 is 1
// the frame one starts.
typedef struct {
	ColsfxStitch* enter[2];   // +0xB4
	ColsfxStitch* exit[2];    // +0xE4
	int32_t enterP[2][5];     // +0xBC
	int32_t exitP[2][5];      // +0xEC
	const ColsfxStitch* latest;   // +0x114: the last one started
} DriveBy;

static DriveBy s_db;
// The LastRandom statics: the going-in stitch's next of three, into a
// tunnel or under an overpass, and the coming-out one's.
static int s_dbTunnelN, s_dbOverN, s_dbExitN;   // r13 - 0x7774 / 0x7770 / 0x776C

static int32_t dbQ15(int output)  { return mixmapQ15(MIXMAP_SFX_DRIVEBY, output); }
static int32_t dbAzim(int output) { return mixmapAzim(MIXMAP_SFX_DRIVEBY, output); }

static void tnReset(void)
{
	for (int i = 0; i < 2; i++) {
		colsfxStitchFree(s_db.enter[i]);
		colsfxStitchFree(s_db.exit[i]);
	}
	memset(&s_tn, 0, sizeof(s_tn));
	memset(&s_db, 0, sizeof(s_db));
	// The constructor's SND_Params: full volume, unit pitch.
	for (int i = 0; i < 2; i++) {
		s_db.enterP[i][1] = s_db.exitP[i][1] = 0x7fff;
		s_db.enterP[i][2] = s_db.exitP[i][2] = 0x1000;
	}
	int32_t* o = mixmapObjOut(MIXMAP_CTL_TUNNEL);
	if (o) o[0] = o[1] = o[2] = o[3] = 0;
	o = mixmapObjOut(MIXMAP_SFX_DRIVEBY);
	if (o) o[5] = 0;
}

static int dbSlot(ColsfxStitch* const w[2]) { return !w[0] ? 0 : !w[1] ? 1 : -1; }

// UpdateParams (0x800A20B0), before the map reads +0x18. The stitch row is
// picked by WooshVol_vs_Vel / 128, which, the slope being 0..1, is always
// row 0 (as retail plays it).
static void dbUpdateParams(const CarSfxIn* in)
{
	int32_t* o = mixmapObjOut(MIXMAP_SFX_DRIVEBY);
	if (s_tn.enter) {
		const int k = dbSlot(s_db.enter);
		if (k >= 0) {
			int* n = s_tn.aheadType == 5 ? &s_dbTunnelN : &s_dbOverN;
			*n %= 3;
			const int idx = (int)(s_tn.enterVol * (1.0f / 128.0f) * 3.0f) * 3 + (*n)++ + 0x2D;
			int32_t* p = s_db.enterP[k];
			p[1] = p[2] = 0;
			s_db.enter[k] = colsfxStitchPlay(1, idx, p);
			if (o) o[5] = 1;
			s_db.latest = s_db.enter[k];
			if (s_tnLogs < 40) {
				s_tnLogs++;
				sfxLog("drive-by: in, zone type %d ahead, %.1f m/s -> stitch %d %s", s_tn.aheadType,
				       in->speed, idx, s_db.enter[k] ? "playing" : "(none)");
			}
		}
	}
	if (s_tn.exit) {
		const int k = dbSlot(s_db.exit);
		if (k >= 0) {
			int32_t* p = s_db.exitP[k];
			p[0] = 4;
			s_dbExitN %= 3;
			const float v = s_tn.exitVol * (1.0f / 128.0f);
			const int idx = (int)(v + v) * 3 + s_dbExitN++ + 0x36;
			p[1] = p[2] = 0;
			s_db.exit[k] = colsfxStitchPlay(1, idx, p);
			if (o) o[5] = 1;
			s_db.latest = s_db.exit[k];
			if (s_tnLogs < 40) {
				s_tnLogs++;
				sfxLog("drive-by: out, %.1f m/s -> stitch %d %s", in->speed, idx,
				       s_db.exit[k] ? "playing" : "(none)");
			}
		}
	}
}

// One woosh's ProcessUpdate: its level (and its azimuth, while it is the last
// one started, or always for one coming out) off its map output, pitch 0x1000
// through AdjustPitch; gone once it has stopped.
static void dbStitchUpdate(ColsfxStitch** w, int32_t* p, bool followAzim)
{
	if (!*w) return;
	if (!colsfxStitchPlaying(*w)) {
		colsfxStitchFree(*w);
		*w = NULL;
		return;
	}
	if (followAzim) p[3] = dbAzim(p[0]);
	p[1] = dbQ15(p[0]);
	p[2] = 0x1000;
	adjustPitch(&p[2]);
	colsfxStitchUpdate(*w, p);
}

// ProcessUpdate (0x800A23B0), after the map.
static void dbProcessUpdate(void)
{
	int32_t* o = mixmapObjOut(MIXMAP_SFX_DRIVEBY);
	// Debug: the levels the map gave a woosh the frame it started.
	if (o && o[5] && s_tnLogs < 40) {
		s_tnLogs++;
		sfxLog("  its mix: in q15 %d azim %04x, out q15 %d azim %04x", (int)dbQ15(0),
		       (unsigned)dbAzim(0), (int)dbQ15(4), (unsigned)dbAzim(4));
	}
	if (o) o[5] = 0;
	for (int i = 0; i < 2; i++) {
		dbStitchUpdate(&s_db.enter[i], s_db.enterP[i], s_db.latest == s_db.enter[i]);
		dbStitchUpdate(&s_db.exit[i], s_db.exitP[i], true);
	}
}

// ---------------------------------------------------------------- roadside wooshes

// CSTATEMGR_DriveBy, the DriveBy state (6): four CSTATE_DriveBy one-shots
// (EnterWorld 0x800AAAB8; ctor 0x800ACA9C, AttachOneShot 0x800ACB6C,
// DetachOneShot 0x800ACBD0), each with an SFXObj_Woosh (SFX 0, 0x800A2C70 on)
// and an SFXCTL_3DWooshPos (SFXCTL 0) on the state's +0x50. A state is
// attached by a lamp post's or a tree's sound sphere the player drives
// through (ProcessSoundSphere) or an opponent passing close
// (UpdatAIDriveBy); its woosh is one MB_Stitch_Whoosh stitch (colsfx.c type
// 1), and the state lets go once the stitch is done.
#define WOOSH_PILLAR 5   // LAMPPOST, PILLAR, TRAFFIC_LIGHT, FREEWAY_SIGN
#define WOOSH_TREE   1
#define WOOSH_AICAR  9   // an opponent going by

// ProcessSoundSphere's names, bStringHash'd.
#define SPHERE_LAMPPOST      0xF40A48EFu
#define SPHERE_PILLAR        0x72F66B23u
#define SPHERE_TRAFFIC_LIGHT 0xB6BD9C95u
#define SPHERE_FREEWAY_SIGN  0xF3ABE1C2u
#define SPHERE_TREE          0x001D5D4Fu

// What a state is attached with (the AttachOneShot data, 0x18 bytes).
typedef struct {
	int   type;     // +0x00
	int   ai;       // +0x04, the EAXCar: -1 the player's, else the opponent's copy
	float speed;    // +0x08
	float pos[3];   // +0x0C
} WooshData;

typedef struct {
	bool    active;     // OneShotBase +0x3C
	int     type;       // +0x44
	int     ai;         // +0x48
	float   speed;      // +0x4C
	float   pos[3];     // +0x50, what 3DWooshPos follows
	ColsfxStitch* w;    // SFXObj_Woosh +0xC8, the "AUD:PillerStich_Wrap"
	int32_t p[5];       // +0xB4, its SND_Params
} Woosh;

static Woosh s_wo[MIXMAP_DRIVEBY_COPIES];
// The LastRandom statics: a pillar's (or a passing car's) next of six, and
// a tree's.
static int s_woPillarN, s_woTreeN;   // r13 - 0x7768 / 0x7764
// Where the player is (the closest player car, Car +0x58 / +0x68) and how
// fast (GetVelocityMagnitudeMPH: Car +0x440 * 2.237), and each opponent
// (the type 9 state's car), as carsfxPreMix and carsfxAiPreMix last saw them.
static struct {
	float pos[3], vel[3], mph;
} s_woPlayer;
static float s_woAiPos[ENGINECTL_AI_MAX][3];
static int s_woLogs;   // debug: the first few wooshes

static uint32_t woId(uint32_t id, int copy) { return MIXMAP_COPY_ID(id, copy); }

static void woReset(void)
{
	for (int i = 0; i < MIXMAP_DRIVEBY_COPIES; i++) colsfxStitchFree(s_wo[i].w);
	memset(s_wo, 0, sizeof(s_wo));
	for (int i = 0; i < MIXMAP_DRIVEBY_COPIES; i++) {
		// SFXObj_Woosh's constructor: full volume, unit pitch.
		s_wo[i].p[1] = 0x7fff;
		s_wo[i].p[2] = 0x1000;
		int32_t* o = mixmapObjOut(woId(MIXMAP_SFX_WOOSH, i));
		if (o) o[6] = o[7] = 0;
	}
	memset(&s_woPlayer, 0, sizeof(s_woPlayer));
	memset(s_woAiPos, 0, sizeof(s_woAiPos));
}

// CSTATEMGR_DriveBy::GetFreeState (0x800AAB18): nothing while a woosh is
// already sounding within half a metre of this one, nor, but for a passing
// car, under 30 mph; else the first state at rest.
static Woosh* woFreeState(const WooshData* d)
{
	for (int i = 0; i < MIXMAP_DRIVEBY_COPIES; i++) {
		const Woosh* s = &s_wo[i];
		if (!s->active) continue;
		const float dx = s->pos[0] - d->pos[0], dy = s->pos[1] - d->pos[1], dz = s->pos[2] - d->pos[2];
		if (sqrtf(dx * dx + dy * dy + dz * dz) < 0.5f) return NULL;
	}
	if (d->type != WOOSH_AICAR && s_woPlayer.mph < 30.0f) return NULL;
	for (int i = 0; i < MIXMAP_DRIVEBY_COPIES; i++)
		if (!s_wo[i].active) return &s_wo[i];
	return NULL;
}

// CSTATE_DriveBy::AttachOneShot, then SFXObj_Woosh::NewAttachment
// (0x800A302C): the stitch by WooshVol_vs_Vel of the speed -- a pillar's
// from 9 on, a tree's from 0x1B, a row of six for each third of the level,
// the next of the row in turn -- and its level, out +0x20, which the map's
// envelope (evt 0) takes when out +0x1C starts it.
static void woAttach(Woosh* s, const WooshData* d)
{
	const int copy = (int)(s - s_wo);
	s->type = d->type;
	s->ai = d->ai;
	s->speed = d->speed;
	memcpy(s->pos, d->pos, sizeof(s->pos));
	s->active = true;
	float v = tnWooshVol(s->speed);
	v = fminf(fmaxf(v, 0.0f), 0.99f);
	const float vol = fminf(fmaxf(v * 127.0f, 0.0f), 127.0f);
	const int row = (int)(vol * 0.0078125f * 3.0f);
	int idx;
	if (s->type == 2 || s->type == 3 || s->type == 5 || s->type == 6 || s->type == WOOSH_AICAR) {
		s_woPillarN %= 6;
		idx = row * 6 + s_woPillarN++ + 9;
	} else {
		s_woTreeN %= 6;
		idx = row * 6 + s_woTreeN++ + 0x1B;
	}
	s->p[1] = s->p[2] = s->p[3] = 0;
	s->w = colsfxStitchPlay(1, idx, s->p);
	int32_t* o = mixmapObjOut(woId(MIXMAP_SFX_WOOSH, copy));
	if (o) {
		o[6] = 1;
		o[7] = clampi((int32_t)vol << 8, 0, 0x7fff) * mixmapMasterVolume(4) >> 15;
	}
	if (s_woLogs < 40) {
		s_woLogs++;
		sfxLog("woosh %d: type %d%s at %.1f,%.1f,%.1f, %.1f m/s -> stitch %d, level %d %s", copy,
		       s->type, s->type == WOOSH_AICAR ? " (opponent)" : "", s->pos[0], s->pos[1], s->pos[2],
		       s->speed, idx, o ? (int)o[7] : -1, s->w ? "playing" : "(none)");
	}
}

static void woMessage(const WooshData* d)
{
	Woosh* s = woFreeState(d);
	if (s) woAttach(s, d);
}

// EAXTunerCar::ProcessSoundSphere (0x8007F404), for each sound sphere
// Car::TriggerEvents set off: a lamp post, a pillar, a traffic light or a
// freeway sign is a pillar's woosh, a tree a tree's, at the sphere's centre,
// as fast as the car goes. An INTERSECTION's sphere looks up the car's route
// point and does nothing with it; TUNNEL_ENTRY and anything else do nothing.
// (It also leaves the centre and 14.01 on the EAXTunerCar, +0x5C and +0x6C,
// which nothing ported reads.) Only in game state 4: not under the pause.
static void woSoundSpheres(const CarSfxIn* in)
{
	if (in->paused) return;
	for (int i = 0; i < in->nEvents; i++) {
		const EmEvent* e = in->events[i];
		if (e->eventId != EM_EVENT_SOUND_SPHERE) continue;
		WooshData d = { 0, -1, in->speed, { e->pos[0], e->pos[1], e->pos[2] } };
		switch (e->nameHash) {
		case SPHERE_LAMPPOST:
		case SPHERE_PILLAR:
		case SPHERE_TRAFFIC_LIGHT:
		case SPHERE_FREEWAY_SIGN:
			d.type = WOOSH_PILLAR;
			break;
		case SPHERE_TREE:
			d.type = WOOSH_TREE;
			break;
		default:
			continue;
		}
		woMessage(&d);
	}
}

// EAXAITunerCar::UpdatAIDriveBy (0x8007EF6C), from its UpdateParams: an
// opponent within 8 m of the player, closing at 15 m/s or more, is a passing
// car's woosh, as loud as it closes, following the opponent.
static void woAiDriveBy(int copy, const CarSfxAiIn* ai)
{
	const CarSfxIn* a = &ai->car;
	const float dx = s_woPlayer.pos[0] - a->pos[0], dy = s_woPlayer.pos[1] - a->pos[1],
		dz = s_woPlayer.pos[2] - a->pos[2];
	if (!(sqrtf(dx * dx + dy * dy + dz * dz) < 8.0f)) return;
	const float rv[3] = { a->vel[0] - s_woPlayer.vel[0], a->vel[1] - s_woPlayer.vel[1],
		a->vel[2] - s_woPlayer.vel[2] };
	const float r2 = rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2];
	const float rel = r2 > 5.0e-11f ? sqrtf(r2) : 0.0f;
	if (rel < 15.0f) return;
	WooshData d = { WOOSH_AICAR, copy, rel, { a->pos[0], a->pos[1], a->pos[2] } };
	woMessage(&d);
}

void carsfxWooshPreMix(void)
{
	// Each state's SFXObj_Woosh::UpdateParams (0x800A2DEC): a passing car's
	// woosh moves with the car; a woosh whose stitch is done is let go, and
	// its state with it (DetachOneShot). PORT: a stitch the pool or the bank
	// could not give is done at once -- retail's wrapper always plays.
	for (int i = 0; i < MIXMAP_DRIVEBY_COPIES; i++) {
		Woosh* s = &s_wo[i];
		if (s->active) {
			if (s->type == WOOSH_AICAR && s->ai >= 0 && s->ai < ENGINECTL_AI_MAX)
				memcpy(s->pos, s_woAiPos[s->ai], sizeof(s->pos));
			if (!colsfxStitchPlaying(s->w)) {
				colsfxStitchFree(s->w);
				s->w = NULL;
				s->active = false;
			}
		}
		mixmapSetObjPos(woId(MIXMAP_CTL_3DWOOSHPOS, i), s->pos);
	}
}

void carsfxWooshUpdate(void)
{
	// SFXObj_Woosh::ProcessUpdate (0x800A2EB4). Its switch on the type means
	// a pillar's woosh to read output 0 and a passing car's output 5, but no
	// case breaks: every woosh ends up on output 3 (-7.75 dB), as retail
	// plays it.
	for (int i = 0; i < MIXMAP_DRIVEBY_COPIES; i++) {
		Woosh* s = &s_wo[i];
		const uint32_t id = woId(MIXMAP_SFX_WOOSH, i);
		int32_t* o = mixmapObjOut(id);
		// Debug: the level the map gave a woosh the frame it started.
		if (o && o[6] && s_woLogs < 40) {
			s_woLogs++;
			sfxLog("  woosh %d's mix: q15 %d azim %04x", i, (int)mixmapQ15(id, 3),
			       (unsigned)mixmapAzim(id, 3));
		}
		if (o) o[6] = 0;
		if (!s->active || !s->w) continue;
		int32_t pitch = 0x1000;
		adjustPitch(&pitch);
		s->p[3] = mixmapAzim(id, 3);
		s->p[1] = mixmapQ15(id, 3);
		s->p[2] = pitch;
		colsfxStitchUpdate(s->w, s->p);
	}
}

// ---------------------------------------------------------------- the car

// GetCarTypeMapping: the car type's CARDATAMAPPING row (CSTATE +0x88).
static int carTypeRow(int carType)
{
	return carType >= 0 && carType < (int)(sizeof(kCarTypeMapping) / sizeof(kCarTypeMapping[0]))
		? kCarTypeMapping[carType] : CAR_TYPE_MAPPING_DEFAULT;
}

// EAXCar::CarPreSetup's tables, SPEED2.EXE's: the car type's row picks the
// engine by the engine level and the shift pattern by the gearbox level.
static void carPreSetup(const CarSfxSetup* s, EngineCtlSetup* o)
{
	const int map = carTypeRow(s->carType);
	const int el = clampi(s->engLevel, 0, 3), tl = clampi(s->transLevel, 0, 3);
	const EngineData* e = &kEngineData[kCarDataMapping[map][el][0]];
	memset(o, 0, sizeof(*o));
	o->engIdx = kCarDataMapping[map][el][0];
	o->eng = e;
	o->dual = &kDualGinsu[e->dualIdx];
	o->shift = &kShiftPatterns[kCarDataMapping[map][tl][1]];
	o->accel = &kAccelFromIdle[e->accelIdx];
	o->engLevel = el;
	o->transLevel = tl;
	o->idle = s->idle;
	o->redline = s->redline;
	o->limiter = s->limiter;
	o->rng = s->rng;
	sfxLog("car type %d -> row %d: engine level %d -> %s, gearbox level %d -> %s",
	       s->carType, map, el, e->spuBank, tl, o->shift->bank);
}

static AemsBank* s_envBank;   // IG_GLOBAL/ENV_COMMON_MB, SFX_Common's
static AemsBank* s_trafBank;  // IG_GLOBAL/TRAFFIC_MB, the traffic's engines
static void trReset(void);
static void trShutdown(void);

void carsfxSetup(const char* audioDir, const CarSfxSetup* s)
{
	carsfxFePurgeClose();   // the garage's NOS bank goes with the frontend
	const int nosUpgrade = s->nosUpgrade;
	carsfxShutdown();
	char path[512];
	// The mix map, which EAXSound::InitializeInGame loads with the world's
	// sounds. Its outputs carry the volume sliders already (SFXCTL_MasterVol),
	// so a bank it drives plays on its own bus; without it, on the effects
	// slider. GetMixMap (0x8007AB18) picks MAPOUTPUT for free roam and the
	// circuit and sprint races; a drift, drag or URL race has its own, by
	// szMixMapFiles (0x80356C28).
	static const char* const kMixMap[4] = {
		"MAPOUTPUT", "MAPOUTPUTDFT", "MAPOUTPUTDRG", "MAPOUTPUTURL",
	};
	const char* mapName = kMixMap[(unsigned)s->mixMap < 4 ? s->mixMap : 0];
	snprintf(path, sizeof(path), "%sMIXMAPS/%s.mxb", audioDir, mapName);
	const bool map = mixmapInit(path);
	sfxLog("mix map %s%s", mapName, map ? "" : " -- not loaded");
	const AudioBus bus = map ? AUDIO_BUS_MIXMAP : AUDIO_BUS_SFX;
	// The engine: CarPreSetup, the controllers' SetupCTL / InitCTL (after the
	// map, whose load clears their outputs), then the SFX.
	EngineCtlSetup ec;
	carPreSetup(s, &ec);
	enginectlSetup(&ec);
	engSetup(audioDir, ec.eng, ec.dual, map ? AUDIO_BUS_MIXMAP : AUDIO_BUS_ENGINE);
	// The shift and the turbo, the PlayerCar state's SFX 1 and 2.
	shiftSetup(audioDir, ec.shift, ec.engIdx, bus);
	// The backfire pops, SFX 4, off the shift's sweetener bank.
	scSetup(ec.engIdx);
	s_rngOwn = 0x12345678u;
	s_rng = s->rng ? s->rng : &s_rngOwn;
	turboSetup(audioDir, carTypeRow(s->carType), s->turboLevel, s->boost, bus);
	// The skids, SFX 5, and their two controllers.
	skidsSetup(audioDir, carTypeRow(s->carType), s->drift,
	           map ? AUDIO_BUS_MIXMAP : AUDIO_BUS_TIRE);
	// Road and wind noise, SFX 6 and 7, and the weather's wind, SFX 10.
	roadWindSetup(audioDir, map ? AUDIO_BUS_MIXMAP : AUDIO_BUS_TIRE, bus);
	// The rain, SFX 9 (its thunder is the NIS module's, speech.h).
	rsSetup(audioDir, bus);
	// SFX_Common::SetupLoadData (0x800AF480), the Main state's banks. The
	// port loads ENV_COMMON_MB (ENV_STATIC: the opponents' and the traffic's
	// stereos, the traffic's horns) and TRAFFIC_MB (FX_TRAFFIC, the traffic's
	// engines) here; MB_Stich_Collision, MB_Stitch_Whoosh and FX_Misc_MB are
	// colsfx.c's and FE_COMMON_MB is FESFX.BIN. ENV_STATIC also plays the
	// world's planes, trains and fountains (envsfx.c, #509); FX_MAIN_MEM_MB,
	// which a URL race loads for its planes, is envsfx.c's too.
	snprintf(path, sizeof(path), "%sIG_GLOBAL/ENV_COMMON_MB.aem", audioDir);
	s_envBank = aemsLoad(path, bus);
	if (!s_envBank)
		sfxLog("ENV_COMMON_MB: no %s -- the stereos and the traffic's horns silent (tools/abk3ds.py)", path);
	snprintf(path, sizeof(path), "%sIG_GLOBAL/TRAFFIC_MB.aem", audioDir);
	s_trafBank = aemsLoad(path, bus);
	if (!s_trafBank)
		sfxLog("TRAFFIC_MB: no %s -- the traffic's engines silent (tools/abk3ds.py)", path);
	envsfxSetup(audioDir, bus, s->mixMap == 3);
	// The landings and bottom-outs, SFX 11, the tunnel controller with its
	// drive-bys, SFX 12, and the DriveBy state's wooshes (stitches in
	// colsfx's banks).
	boReset();
	tnReset();
	woReset();
	// The Traffic state's six (CSTATEMGR_TrafficCar::EnterWorld).
	trReset();
	// SetupLoadData: g_NOS bank names, [1] for upgrade levels 2 and 3.
	static const char* const kNosBank[2] = { "Nitrous_00_MB", "Nitrous_01_MB" };
	const int which = (nosUpgrade == 2 || nosUpgrade == 3) ? 1 : 0;
	snprintf(path, sizeof(path), "%sNOS/%s.aem", audioDir, kNosBank[which]);
	s_nos.upgrade = nosUpgrade;
	s_nos.sfxId = MIXMAP_SFX_NITROUS;
	s_nos.bank = aemsLoad(path, bus);
	if (s_nos.bank)
		sfxLog("nitrous upgrade %d -> %s", nosUpgrade, kNosBank[which]);
	else
		sfxLog("nitrous upgrade %d -> %s: no %s -- nitrous silent (tools/abk3ds.py)",
		     nosUpgrade, kNosBank[which], path);
	// CARSFX_Hydraulics' bank (#313).
	hyShutdown();
	snprintf(path, sizeof(path), "%sNOS/Hydraulics_00_MB.aem", audioDir);
	s_hy.bank = aemsLoad(path, bus);
	if (s_hy.bank)
		sfxLog("hydraulics: Hydraulics_00_MB loaded, mix map object %s",
		     mixmapObjOut(MIXMAP_SFX_HYDRAULICS) ? "registered" : "missing");
	else
		sfxLog("hydraulics: no %s -- the hydraulics silent (tools/abk3ds.py)", path);
}

void carsfxPreMix(const CarSfxIn* in, float dt)
{
	// The world's step comes before EAXSound::Update: the sound spheres
	// Car::TriggerEvents set off attach their wooshes now.
	memcpy(s_woPlayer.pos, in->pos, sizeof(s_woPlayer.pos));
	memcpy(s_woPlayer.vel, in->vel, sizeof(s_woPlayer.vel));
	s_woPlayer.mph = in->speed * 2.237f;
	woSoundSpheres(in);
	enginectlUpdate(&in->eng, dt);
	// SFXCTL_Wheel (1) and SFXCTL_Skid (2), which only each other read.
	if (s_sk.p) {
		wheelUpdate(&s_sk, in);
		skidCtlUpdate(&s_sk, in, dt);
	}
	// CARSFX_Shift::UpdateMixerOutputs: its output +0x1C is 1 while Shifting
	// is in an up-shift's state 1. (Written with the controllers' outputs, so
	// the map reads this frame's.)
	int32_t* o = mixmapObjOut(MIXMAP_SFX_SHIFT);
	if (o) o[6] = enginectlMix()->shiftState == 1;
	// Car::TriggerCarTerrainEffects / ChangeGear's SFXMessage 9 (EAXTunerCar
	// 0x8007F68C sets CSTATE +0xE8), and CARSFX_SparkChatter's outputs.
	if (in->backfire) s_sc.pending = true;
	scUpdateMixerOutputs();
	rsUpdateMixerOutputs();
	// SFXCTL_Tunnel (7), then CARSFX_BottomOut's and CARSFX_DriveBy's
	// UpdateParams, whose outputs the map's envelopes read.
	tnUpdate(in, dt);
	boUpdateParams(in, dt);
	dbUpdateParams(in);
}

void carsfxUpdate(const CarSfxIn* in, float dt)
{
	s_pausedFor = (in->paused && s_paused) ? s_pausedFor + dt : 0.0f;
	s_paused = in->paused;
	// The PlayerCar state's SFX in ID order: the engine (0), the shift (1),
	// the turbo (2), nitrous (3), the backfire (4), the skids (5), road noise
	// (6), wind (7), the landings (11), then the drive-bys (12).
	const EngineCtlMix* m = enginectlMix();
	engSetEngineParams();
	shiftUpdateParams(m, in->eng.povType);
	shiftProcessUpdate(in->paused);
	turboUpdateParams(&s_tb, m, dt, in->paused);
	turboProcessUpdate(&s_tb, m);
	if ((s_tbReportIn -= dt) <= 0.0f) {
		s_tbReportIn = 5.0f;
		if (s_tb.spool)
			sfxLog("turbo: state %d, spool %.1f / %.0f (boost %.2f%s), volume %d, refcount %d",
			       s_tb.state, s_tb.spoolAmt, s_tb.ds->maxSpool, s_tb.boost, s_tb.capped ? ", capped" : "",
			       (int)s_tb.spoolData[1], aemsRefCount(s_tb.spool));
		// Debug: the engine's mix and what NDSP is actually sounding.
		sfxLog("engine: q15 %d azim %d, Ginsu vol %d/%d peak %d/%d, CAR c4 %d c25 %d, %d AEMS voice(s)",
		       (int)engQ15(), (int)engAzim(), (int)s_eng.vol[0], (int)s_eng.vol[1],
		       (int)ginsuTakePeak(&s_eng.syn[0]), (int)ginsuTakePeak(&s_eng.syn[1]),
		       (int)s_eng.carData[4], (int)s_eng.carData[25], aemsVoicesPlaying());
		for (int ch = 0; ch < 24; ch++) {
			char b[128];
			if (audioDebugChannel(ch, b, sizeof(b))) sfxLog("  %s", b);
		}
		if (s_rs.inst || s_rs.stopping)
			sfxLog("rain: intensity %.2f, member vol %d (q15 out %d/%d)%s, refcount %d, %d thunder ask(s)",
			       s_rs.rain, (int)s_rs.data[2], (int)mixmapQ15(MIXMAP_SFX_RAIN, 0),
			       (int)mixmapQ15(MIXMAP_SFX_RAIN, 1), s_rs.stopping ? ", stopping" : "",
			       aemsRefCount(s_rs.inst), s_rs.thunders);
		char mb[192];
		if (audioDebugMix(mb, sizeof(mb))) sfxLog("  %s", mb);
		if (audioDebugDsp(mb, sizeof(mb))) sfxLog("  %s", mb);
	}
	nosUpdateParams(&s_nos, in);
	nosProcessUpdate(&s_nos);
	// The hydraulics (8).
	hyUpdateParams(in);
	hyProcessUpdate();
	// The backfire (4).
	scUpdateParams();
	scProcessUpdate(m);
	// The skids (5).
	skidsUpdateParams(&s_sk);
	skidsProcessUpdate(&s_sk, in);
	skidsReport(in, dt);
	// Road noise (6) and wind (7).
	if (s_rn.enabled) {
		rnGenerate(in);
		rnGenTerrain();
	}
	rnProcessUpdate();
	wnUpdateParams(in);
	wnUpdateCsis();
	// The rain (9).
	rsUpdateParams(in, in->paused ? 0.0f : dt);
	rsProcessUpdate(in);
	// The weather's wind (10).
	wwUpdateParams(in);
	wwProcessUpdate();
	roadWindReport(dt);
	// The landings and bottom-outs (11), then the drive-bys (12).
	boProcessUpdate();
	dbProcessUpdate();
	if (s_reportIn > 0.0f && (s_reportIn -= dt) <= 0.0f)
		sfxLog("%d voice(s) sounding; FX_NITROUS refcount %d, FX_PURGE refcount %d",
		       aemsVoicesPlaying(), s_nos.nos ? aemsRefCount(s_nos.nos) : 0,
		       s_nos.purge ? aemsRefCount(s_nos.purge) : 0);
}

void carsfxShutdown(void)
{
	carsfxAiShutdown();   // their instances live in the player's bank too
	trShutdown();         // the traffic's too, and TRAFFIC_MB
	envsfxShutdown();     // the world's, and FX_MAIN_MEM_MB
	if (s_envBank) aemsUnload(s_envBank);
	s_envBank = NULL;
	letGo(&s_nos.nos);
	letGo(&s_nos.purge);
	if (s_nos.bank) aemsUnload(s_nos.bank);
	hyShutdown();
	engShutdown();
	scShutdown();   // before the shift's sweetener bank goes
	shiftShutdown();
	turboShutdown(&s_tb);
	skidsShutdown(&s_sk);
	roadWindShutdown();
	rsShutdown();
	boReset();
	tnReset();
	woReset();
	enginectlSetup(NULL);
	mixmapShutdown();
	s_nos = (Nitrous){ 0 };
	s_paused = false;
	s_pausedFor = 0.0f;
	s_reportIn = 0.0f;
	s_tbReportIn = 0.0f;
	s_skReportIn = 0.0f;
	s_skEventIn = 0.0f;
	s_rwReportIn = 0.0f;
}

// ---------------------------------------------------------------- AI engines

// EngGroup1, EngGroup2 and EngGroup3: the eSND_ENGINE_DATA entries (kEngineData
// indices) of engine groups 0, 1 and 2, which GenRandomEngineFromGroup
// (0x8007EAFC) draws from and IsEngBankInGroup (0x8007EBB8) searches.
static const int8_t kEngGroup0[] = { 0, 1, 2, 3, 4, 5, 10, 11, 18, 20, 21, 23, 24, 34 };
static const int8_t kEngGroup1[] = { 1, 2, 6, 15, 16, 18, 20, 21, 24, 25, 28 };
static const int8_t kEngGroup2[] = { 12, 13, 9, 15 };

static int engGroup(int g, const int8_t** t)
{
	switch (g) {
	case 0: *t = kEngGroup0; return (int)sizeof(kEngGroup0);
	case 1: *t = kEngGroup1; return (int)sizeof(kEngGroup1);
	case 2: *t = kEngGroup2; return (int)sizeof(kEngGroup2);
	default: *t = NULL; return 0;
	}
}

// CARSFX_AIEngine: one opponent's CAR instance.
typedef struct {
	bool       on;
	const EngineData* eng;   // CSTATE +0x7C, the engine ResolveEngineBankLoading gave it
	AemsInst   car;          // +0xB0's instance
	int32_t    carData[26];  // ... its member data
	int32_t    pitchOffset;  // +0xC0: -300 .. 200, one step on per engine made
} AiEngine;

static AiEngine   s_ai[ENGINECTL_AI_MAX];
// The AICar state's SFX 2, 3 and 5: CreateSFX (0x800A913C) falls back to the
// PlayerCar state's classes, and they play through the player's banks.
static Turbo      s_aiTb[ENGINECTL_AI_MAX];
static Nitrous    s_aiNos[ENGINECTL_AI_MAX];
static Skids      s_aiSk[ENGINECTL_AI_MAX];

// CARSFX_Stereo (0x800A81CC..0x800A8740), the AICar state's SFX 15: the
// opponent's car stereo, CSIS ENV_STATIC out of ENV_COMMON_MB (SFX_Common's
// bank, carsfxSetup).
typedef struct {
	bool      enabled;       // +0xA0
	uint8_t   track;         // +0xB8: 8 + bRandom(2)
	AemsInst  inst;          // +0xB0's instance
	int32_t   data[5];       // ... its member data
} AiStereo;

static AiStereo   s_aiSt[ENGINECTL_AI_MAX];
static AemsClass  s_aiStereoCls;    // ENV_STATIC
// r13 - 0x7724: InitSFX's count of the racers' stereos, for the session.
static int        s_aiStereoCount;
static int        s_aiCount;
static AemsBank*  s_aiBank[3];      // the banks the opponents loaded ...
static const char* s_aiBankName[3]; // ... by their spuBank
static uint32_t*  s_aiRng;
static uint32_t   s_aiRngOwn;
static float      s_aiReportIn;
// r13 - 0x7784: the ctor's pitch step, which every CARSFX_AIEngine made moves
// on by 100 (mod 600) for the whole session.
static int32_t    s_aiPitchStep;

// GetCartypeEngGroup (0x8007EC2C): a car type's CARDATAMAPPING row to its
// engine group, 0 .. 2.
static int cartypeEngGroup(int row)
{
	switch (row) {
	case 9: case 0xd: case 0xe: case 0x14: case 0x15:
		return 2;
	case 1: case 2: case 8: case 0xb: case 0xf: case 0x10: case 0x11:
	case 0x1b: case 0x1d: case 0x1e: case 0x1f: case 0x20:
		return 1;
	default:
		return 0;
	}
}

// GenRandomEngineFromGroup (0x8007EAFC): bRandom over the group's list.
static int randomEngineFromGroup(int g)
{
	const int8_t* t;
	const int n = engGroup(g, &t);
	return n ? t[rideinfo_random(s_aiRng, (uint32_t)n)] : 0;
}

// IsEngBankInGroup (0x8007EBB8).
static bool engInGroup(int g, int eng)
{
	const int8_t* t;
	const int n = engGroup(g, &t);
	for (int i = 0; i < n; i++)
		if (t[i] == eng) return true;
	return false;
}

// IsEngineInLoadedArray (0x800AA380): its slot of the three, or -1.
static int engLoadedAt(const int* loaded, int eng)
{
	for (int i = 0; i < 3; i++)
		if (loaded[i] == eng) return i;
	return -1;
}

// A random engine of the group not yet loaded, giving up after 101 draws.
static int randomUnloadedEngine(int g, const int* loaded)
{
	int e = 0;
	for (int t = 0; t <= 100; t++) {
		e = randomEngineFromGroup(g);
		if (engLoadedAt(loaded, e) == -1) break;
	}
	return e;
}

// CSTATEMGR_AICar::ResolveEngineBankLoading (0x800AA3B4): the race's
// opponents share at most three engines. Each engine group present gets one
// -- a car's own engine where it is of its group, else a random one of the
// group -- then any slot left over goes to the groups in turn; a car without
// one of its own then takes the loaded engines of its group in turn. `eng` in
// is each car's own engine (CarPreSetup's), out the one it plays.
static void resolveEngineBankLoading(int n, const int* group, int* eng)
{
	int present[3] = { 0, 0, 0 };
	int loaded[3] = { -1, -1, -1 };          // local_58[0..2]
	int carEng[ENGINECTL_AI_MAX];            // local_58[4 + copy]
	for (int i = 0; i < n; i++) {
		carEng[i] = -1;
		present[group[i]] = 1;
	}
	int got = 0;
	for (int i = 0; i < n; i++) {
		const int g = group[i];
		if (present[g] && loaded[g] == -1 && engInGroup(g, eng[i]) &&
		    engLoadedAt(loaded, eng[i]) == -1) {
			loaded[g] = carEng[i] = eng[i];
			if (++got == 3 || got == n) goto done;
		}
	}
	for (int i = 0; i < n; i++) {
		if (loaded[group[i]] != -1) continue;
		got++;
		loaded[group[i]] = carEng[i] = randomUnloadedEngine(group[i], loaded);
		if (got == 3 || got == n) goto done;
	}
	{
		int gi = 0;
		for (int k = 0; k < 3; k++) {
			if (loaded[k] != -1) continue;
			bool placed = false;
			// Port guard: retail cycles the groups until a car is found.
			for (int round = 0; !placed && round < 3; round++) {
				while (!present[gi]) gi = (gi + 1) % 3;
				for (int i = 0; i < n; i++) {
					if (group[i] != gi || carEng[i] != -1) continue;
					int e = eng[i];
					if (!engInGroup(gi, e) || engLoadedAt(loaded, e) != -1)
						e = randomUnloadedEngine(group[i], loaded);
					got++;
					loaded[k] = carEng[i] = e;
					placed = true;
					if (got == 3 || got == n) goto done;
					break;
				}
				gi = (gi + 1) % 3;
			}
			if (!placed) break;
		}
	}
done:;
	int k = 0;
	for (int i = 0; i < n; i++) {
		if (carEng[i] != -1) {
			eng[i] = carEng[i];
			continue;
		}
		// Port guard: retail always finds one of the car's group.
		for (int t = 0; t < 3 && !engInGroup(group[i], loaded[k]); t++) k = (k + 1) % 3;
		if (loaded[k] != -1) eng[i] = loaded[k];
		k = (k + 1) % 3;
	}
}

// SetupLoadData (0x800AF27C), with TestForDuplicateAEMSBanks: the engine's
// CAR bank, once however many cars play it -- and not at all when it is the
// player's, which is loaded already.
static bool aiLoadBank(const char* audioDir, const EngineData* e, AudioBus bus)
{
	if (s_eng.bank && s_eng.eng && strcmp(s_eng.eng->spuBank, e->spuBank) == 0) return true;
	int slot = -1;
	for (int j = 0; j < 3; j++) {
		if (s_aiBankName[j] && strcmp(s_aiBankName[j], e->spuBank) == 0) return s_aiBank[j] != NULL;
		if (!s_aiBankName[j] && slot < 0) slot = j;
	}
	if (slot < 0) return false;   // port guard: three engines, three banks at most
	char path[512];
	bankPath(path, sizeof(path), audioDir, e->spuBank);
	s_aiBankName[slot] = e->spuBank;
	s_aiBank[slot] = aemsLoad(path, bus);
	if (!s_aiBank[slot])
		sfxLog("AI engine: no %s -- its cars silent (tools/abk3ds.py)", path);
	else
		sfxLog("AI engine: %s loaded", path);
	return s_aiBank[slot] != NULL;
}

// CARSFX_AIEngine::SetEngineParams (0x8009F92C): the Engine controller's
// revs on the engine's Ginsu RPM scale (the CAR graph's), its level by the
// mix map's, its torque, and the car's pitch offset and azimuth.
static void aiSetEngineParams(int c)
{
	AiEngine* a = &s_ai[c];
	if (!a->car) return;
	const EngineCtlMix* m = enginectlAiMix(c);
	const uint32_t id = MIXMAP_AICAR_ID(MIXMAP_SFX_AIENGINE, c);
	const int32_t q15 = mixmapQ15(id, 0);
	const int32_t lvl = m->engVol * q15 >> 15;
	const float f = (m->engRpm - 1000.0f) / 9000.0f;
	int32_t* d = a->carData;
	d[4] = clampi(lvl - 0x7fff, -0x7fff, 0);
	d[5] = clampi(lvl - 0x7fff, -0x7fff, 0);
	d[1] = clampi((int32_t)(f * (a->eng->ginMaxRpm - a->eng->ginMinRpm) + a->eng->ginMinRpm), 0, 10000);
	// GenDopplerFactor(eDOPPLER_PARAMS 2) of the car's 3DCarPos (#503).
	const float doppler = mixmapDoppler(MIXMAP_AICAR_ID(MIXMAP_CTL_3DCARPOS, c),
		MIXMAP_DOPPLER_AIENGINE);
	d[16] = clampi((int32_t)((float)(a->pitchOffset + 16000) * doppler - 16000.0f), -0x3fff, 0x3fff);
	d[3] = clampi((int32_t)(m->torque * 1024.0f * 0.01f), 0, 0x400);
	d[17] = clampi(m->f130, 0, 0x400);
	d[15] = clampi(mixmapAzim(id, 0), 0, 0x10000);
	aemsSetMemberData(a->car, d);
}

// CARSFX_Stereo::UpdateParams (0x800A8514): with its Car, an ENV_STATIC
// instance ("AUD: AI STEREO", member data {track, 0, 0, 0, 0}) once the map
// gives the stereo a level, let go when the level is back to 0. Then
// ProcessUpdate (0x800A86C8): [1] the level, [2] 0x1000, [3] the azimuth.
// `id` is the stereo's SFX (an AICar or a Traffic copy's), `who` its name
// in the log.
static void stereoUpdate(AiStereo* st, uint32_t id, bool live, const char* who)
{
	if (st->enabled && live) {
		const int32_t q = mixmapQ15(id, 0);
		if (!st->inst && q != 0) {
			memset(st->data, 0, sizeof(st->data));
			st->data[0] = st->track > 0x32 ? 0x32 : st->track;
			st->inst = aemsCreate(aemsClass("ENV_STATIC"), st->data);
			sfxLog("%sstereo: on, track %d", who, (int)st->track);
		} else if (st->inst && q == 0) {
			letGo(&st->inst);
			sfxLog("%sstereo: off", who);
		}
	}
	if (!st->inst) return;
	st->data[1] = clampi(mixmapQ15(id, 0), 0, 0x7fff);
	st->data[3] = clampi(mixmapAzim(id, 0), 0, 0xffff);
	st->data[2] = 0x1000;
	aemsSetMemberData(st->inst, st->data);
}

static void aiStereoUpdate(int c, bool live)
{
	char who[8];
	snprintf(who, sizeof(who), "AI %d ", c);
	stereoUpdate(&s_aiSt[c], MIXMAP_AICAR_ID(MIXMAP_SFX_AISTEREO, c), live, who);
}

void carsfxAiSetup(const char* audioDir, const CarSfxAiCar* cars, int n, bool drift,
	bool shortTrack, uint32_t* rng)
{
	carsfxAiShutdown();
	if (!cars || n <= 0) return;
	if (n > ENGINECTL_AI_MAX) n = ENGINECTL_AI_MAX;
	s_aiRngOwn = 0x12345678u;
	s_aiRng = rng ? rng : &s_aiRngOwn;
	// EnterWorld (0x800AA0BC): each car's CarPreSetup (EAXCar's, then its
	// engine group), the shared engines, then each car's controllers and SFX
	// on the engine it was given.
	EngineCtlSetup ec[ENGINECTL_AI_MAX];
	int group[ENGINECTL_AI_MAX], eng[ENGINECTL_AI_MAX], own[ENGINECTL_AI_MAX];
	for (int i = 0; i < n; i++) {
		const CarSfxSetup s = {
			.carType = cars[i].carType, .engLevel = cars[i].engLevel,
			.transLevel = cars[i].transLevel, .idle = cars[i].idle,
			.redline = cars[i].redline, .limiter = cars[i].limiter, .rng = s_aiRng,
		};
		carPreSetup(&s, &ec[i]);
		group[i] = cartypeEngGroup(carTypeRow(cars[i].carType));
		own[i] = eng[i] = ec[i].engIdx;
	}
	resolveEngineBankLoading(n, group, eng);
	const AudioBus bus = mixmapReady() ? AUDIO_BUS_MIXMAP : AUDIO_BUS_ENGINE;
	s_aiStereoCls = aemsClass("ENV_STATIC");
	for (int i = 0; i < n; i++) {
		AiEngine* a = &s_ai[i];
		const int ei = clampi(eng[i], 0, (int)(sizeof(kEngineData) / sizeof(kEngineData[0])) - 1);
		const EngineData* e = &kEngineData[ei];
		const bool bank = aiLoadBank(audioDir, e, bus);
		ec[i].engIdx = ei;
		ec[i].eng = e;
		ec[i].dual = &kDualGinsu[e->dualIdx];
		ec[i].accel = &kAccelFromIdle[e->accelIdx];
		ec[i].drift = drift;
		ec[i].shortTrack = shortTrack;
		enginectlAiSetup(i, &ec[i]);
		// The ctor, then InitializeEngine (0x8009FBE0): a CAR instance with
		// every field 0 but the engine ID.
		s_aiPitchStep = (s_aiPitchStep + 100) % 600;
		a->pitchOffset = s_aiPitchStep - 300;
		a->eng = e;
		a->on = true;
		memset(a->carData, 0, sizeof(a->carData));
		a->carData[0] = clampi(e->id, 0, 0x3c);
		if (bank) a->car = aemsCreate(aemsClass("CAR"), a->carData);
		sfxLog("AI %d: group %d, engine %d -> %d (%s, id %d), pitch %+d, CAR instance %s", i,
		       group[i], own[i], ei, e->spuBank, (int)e->id,
		       (int)a->pitchOffset, a->car ? "up" : "none");
		// The turbo (2) by the car's own turbo level and boost curve, nitrous
		// (3), and the skids (5) with the one voice an AI driver gets. Every
		// CARSFX ctor sets +0x54, so CSTATE_Base::LoadData (0x800ABF74) skips
		// their SetupLoadData: the player's banks are loaded already.
		char who[8];
		snprintf(who, sizeof(who), "AI %d ", i);
		const int row = carTypeRow(cars[i].carType);
		Turbo* t = &s_aiTb[i];
		memset(t, 0, sizeof(*t));
		t->sfxId = MIXMAP_AICAR_ID(MIXMAP_SFX_TURBO, i);
		memcpy(t->who, who, sizeof(t->who));
		turboInit(t, row, cars[i].turboLevel, cars[i].boost);
		if (!s_tb.bank) t->enabled = false;   // port guard: no bank, nothing to play
		Nitrous* nn = &s_aiNos[i];
		memset(nn, 0, sizeof(*nn));
		nn->sfxId = MIXMAP_AICAR_ID(MIXMAP_SFX_NITROUS, i);
		memcpy(nn->who, who, sizeof(nn->who));
		// ctlId 0: the AICar state has no SFXCTL_Skid for the map to read.
		skidsInit(&s_aiSk[i], row, drift, 1, MIXMAP_AICAR_ID(MIXMAP_SFX_SKIDS, i), 0, who);
		// The stereo (15). InitSFX (0x800A83F8), the car attached: a racer
		// (DriverInfo type 2, every port opponent) gets one every sixth car.
		// NewAttachment (0x800A82E4) then draws the track again.
		AiStereo* st = &s_aiSt[i];
		memset(st, 0, sizeof(*st));
		st->track = (uint8_t)(8 + rideinfo_random(s_aiRng, 2));
		s_aiStereoCount = (s_aiStereoCount + 1) % 6;
		st->enabled = s_aiStereoCount == 0;
		st->track = (uint8_t)(8 + rideinfo_random(s_aiRng, 2));
		if (!s_aiStereoCls) st->enabled = false;   // port guard: no ENV_COMMON_MB
		if (st->enabled) sfxLog("AI %d stereo: track %d", i, (int)st->track);
	}
	s_aiCount = n;
	s_aiReportIn = 1.0f;
}

void carsfxAiPreMix(const CarSfxAiIn* in, int n, float dt)
{
	for (int c = 0; c < s_aiCount && c < n; c++) {
		if (!s_ai[c].on) continue;
		enginectlAiUpdate(c, &in[c].eng, dt);
		// SFXCTL_Engine::UpdateParams' first part: where the car is, for its
		// 3DCarPos.
		mixmapSetObjPos(MIXMAP_AICAR_ID(MIXMAP_CTL_3DCARPOS, c), in[c].car.pos);
		if (in[c].live) {
			// The skids' levels (the AICar state's SFXCTL_Wheel and Skid work
			// for its CARSFX_Skids, not the map).
			if (s_aiSk[c].p) {
				wheelUpdate(&s_aiSk[c], &in[c].car);
				skidCtlUpdate(&s_aiSk[c], &in[c].car, dt);
			}
			// EAXAITunerCar::UpdateParams: the car going by.
			memcpy(s_woAiPos[c], in[c].car.pos, sizeof(s_woAiPos[c]));
			woAiDriveBy(c, &in[c]);
		}
	}
}

void carsfxAiUpdate(const CarSfxAiIn* in, int n, float dt)
{
	// The AICar state's SFX in ID order, copy by copy: the engine (0), the
	// turbo (2), nitrous (3), the skids (5) and the stereo (15). A copy
	// without a Car only keeps its engine and turbo going, and a stereo
	// already playing.
	for (int c = 0; c < s_aiCount; c++) {
		aiSetEngineParams(c);
		if (!s_ai[c].on) continue;
		const EngineCtlMix* m = enginectlAiMix(c);
		turboUpdateParams(&s_aiTb[c], m, dt, s_paused);
		turboProcessUpdate(&s_aiTb[c], m);
		const bool live = c < n && in[c].live;
		if (live) nosUpdateParams(&s_aiNos[c], &in[c].car);
		nosProcessUpdate(&s_aiNos[c]);
		if (live) {
			skidsUpdateParams(&s_aiSk[c]);
			skidsProcessUpdate(&s_aiSk[c], &in[c].car);
		}
		aiStereoUpdate(c, live);
	}
	// Debug: every 5 s, each opponent's engine, turbo and skids.
	if (s_aiCount > 0 && (s_aiReportIn -= dt) <= 0.0f) {
		s_aiReportIn = 5.0f;
		for (int c = 0; c < s_aiCount; c++) {
			const AiEngine* a = &s_ai[c];
			const Turbo* t = &s_aiTb[c];
			const Skids* sk = &s_aiSk[c];
			const uint32_t id = MIXMAP_AICAR_ID(MIXMAP_SFX_AIENGINE, c);
			sfxLog("AI %d: q15 %d azim %d; CAR rpm %d c4 %d trq %d pitch %d, refcount %d; "
			       "turbo %s st %d boost %.2f vol %d q15 %d; nitrous %s; skid %d/%d/%d q15 %d", c,
			       (int)mixmapQ15(id, 0), (int)mixmapAzim(id, 0), (int)a->carData[1],
			       (int)a->carData[4], (int)a->carData[3], (int)a->carData[16],
			       a->car ? aemsRefCount(a->car) : 0,
			       t->enabled ? "on" : "off", t->state, t->boost, (int)t->spoolData[1],
			       (int)tbQ15(t, 0), s_aiNos[c].nos ? "on" : "-",
			       (int)sk->mags[0][0], (int)sk->mags[0][1], (int)sk->mags[0][2],
			       (int)skQ15(sk, 3));
		}
		sfxLog("AI engines: %d AEMS voice(s) sounding, %d live instance(s)", aemsVoicesPlaying(),
		       aemsLiveInstances());
	}
}

void carsfxAiShutdown(void)
{
	for (int c = 0; c < ENGINECTL_AI_MAX; c++) {
		letGo(&s_ai[c].car);
		turboShutdown(&s_aiTb[c]);
		letGo(&s_aiNos[c].nos);
		letGo(&s_aiNos[c].purge);
		skidsShutdown(&s_aiSk[c]);
		letGo(&s_aiSt[c].inst);
	}
	memset(s_aiNos, 0, sizeof(s_aiNos));
	memset(s_aiSt, 0, sizeof(s_aiSt));
	s_aiStereoCls = NULL;
	for (int j = 0; j < 3; j++)
		if (s_aiBank[j]) aemsUnload(s_aiBank[j]);
	memset(s_ai, 0, sizeof(s_ai));
	memset(s_aiBank, 0, sizeof(s_aiBank));
	memset(s_aiBankName, 0, sizeof(s_aiBankName));
	s_aiCount = 0;
	s_aiReportIn = 0.0f;
	enginectlAiShutdown();
}

// ---------------------------------------------------------------- traffic

// CSTATEMGR_TrafficCar (ctor 0x800A9C68, EnterWorld 0x800A9D20, UpdateParams
// 0x800A9E48, ConnectCar 0x800A9FAC, DisconnectCar 0x800A9F80), the Traffic
// state (3): six CSTATEs, each on a traffic car near the player. A state has
// its car (+0x30), its clock (+0x34) and, by ID, SFXCTL_3DTrafficPos (SFXCTL
// 0, an SFXCTL_3DCarPos on the car), CARSFX_TrafficEngine (SFX 0),
// CARSFX_TrafficWoosh (1), CARSFX_TrafficHorn (2), CARSFX_Stereo (3) and
// CARSFX_BottomOut (4; not ported -- nothing sends a traffic car's the
// message).
#define TR_SND_RADIUS   70.0f   // UpdateParams' IsCarInRadius radius (0x8031F4F8)

typedef struct {
	int      car;           // +0x30: the traffic car's index, -1 none (+0x3C clear)
	CarSfxTrafficCar in;    // what the car was this frame
	// CARSFX_TrafficEngine (ctor 0x800A4130): FX_TRAFFIC, TRAFFIC_MB's.
	AemsInst eng;           // +0xB0's instance ...
	int32_t  engData[5];    // ... its member data
	uint8_t  variant;       // +0xBC
	// CARSFX_TrafficWoosh (ctor 0x800A53A4): an MB_Stitch_Whoosh stitch.
	ColsfxStitch* wsh;      // +0xB4, the "AUD:TrafficWsh_Stch"
	float    wshLast;       // +0xB8: when the last one started
	int32_t  wshP[5];       // its SND_Params
	// CARSFX_TrafficHorn (ctor 0x800A487C): ENV_STATIC, ENV_COMMON_MB's.
	AemsInst horn;          // +0xB4's instance ...
	int32_t  hornData[5];   // ... its member data
	float    hornDur;       // +0xB8: how long this honk holds
	float    hornStart;     // +0xBC
	float    hornEnd;       // +0xC0: when the last one ended
	bool     hornHeld;      // +0xC4: StartHonkHorn's -- an empty stub, so never set
	bool     honking;       // +0xC8
	bool     canHonk;       // +0xCC
	float    hornTry;       // +0xD0: when a honk last lost the draw
	InterpLine hornFade;    // +0xD8: 1, then 1 -> 0 over 150 ms to end
	bool     hornFading;    // +0xF4
	// CARSFX_Stereo.
	AiStereo st;
} TrafficSnd;

static TrafficSnd s_tr[MIXMAP_TRAFFIC_COPIES];
static bool       s_trOn;          // carsfxSetup has run
// CSTATE +0x34: the states' clock, the unpaused time since they were made
// (CSTATE_Base::UpdateParams 0x800AC158). Every copy's runs from EnterWorld,
// so one stands for all six.
static float      s_trClock;
static float      s_trLastHonk;    // r13 - 0x7750: when any traffic car last began one
static int        s_trVariant;     // r13 - 0x774C: NewAttachment's engine variant, 0 / 1 in turn
static int        s_trWooshN;      // r13 - 0x7744: the woosh's next of six
static CarSfxTrafficPlayer s_trPl; // the player, as carsfxTrafficPreMix last saw it
static float      s_trReportIn;
static int        s_trLogs;        // debug: the first few honks and wooshes

static float trDist(const float a[3], const float b[3])
{
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// The ctors and InitSFX, at EnterWorld (carsfxSetup's): no car, the horn
// free to honk, its fade at 1, the stereo's track drawn.
static void trReset(void)
{
	memset(s_tr, 0, sizeof(s_tr));
	for (int c = 0; c < MIXMAP_TRAFFIC_COPIES; c++) {
		TrafficSnd* s = &s_tr[c];
		s->car = -1;
		s->canHonk = true;
		lineInit(&s->hornFade, 1.0f, 1.0f, 1, 0);
		// CARSFX_Stereo::InitSFX (0x800A83F8): the track; with no car yet,
		// not the racer's every-sixth stereo.
		s->st.track = (uint8_t)(8 + rideinfo_random(s_rng, 2));
	}
	s_trClock = 0.0f;
	s_trLastHonk = 0.0f;
	s_trVariant = 0;
	s_trWooshN = 0;
	memset(&s_trPl, 0, sizeof(s_trPl));
	s_trReportIn = 5.0f;
	s_trLogs = 0;
	s_trOn = true;
}

// GetPlayerSpeedRelativeToUs (0x800A5810): how fast the player goes by,
// flat: |player's velocity - the car's| in x and y.
static float trRelSpeed(const TrafficSnd* s)
{
	const float dx = s_trPl.vel[0] - s->in.vel[0], dy = s_trPl.vel[1] - s->in.vel[1];
	const float r2 = dx * dx + dy * dy;
	return r2 > 5.0e-11f ? sqrtf(r2) : 0.0f;
}

// CARSFX_TrafficHorn::CSIS_EndHonk (0x800A4E44): let go, and the next honk
// waits 3 s from now.
static void trEndHonk(TrafficSnd* s)
{
	letGo(&s->horn);
	s->hornHeld = false;
	s->honking = false;
	s->hornFading = false;
	s->hornEnd = s_trClock;
}

// CSIS_BeginHonk (0x800A4CC4): "AUD: Traffic Horn", an ENV_STATIC instance
// of `sample` (0..0x32) at no level yet, ProcessUpdate's to set.
static void trBeginHonk(TrafficSnd* s, int sample)
{
	letGo(&s->horn);
	memset(s->hornData, 0, sizeof(s->hornData));
	s->hornData[0] = clampi(sample, 0, 0x32);
	if (s_envBank) s->horn = aemsCreate(aemsClass("ENV_STATIC"), s->hornData);
	s_trLastHonk = s_trClock;
	s->hornStart = s_trClock;
	lineInit(&s->hornFade, 1.0f, 1.0f, 1, 0);
	s->hornFading = false;
	// Port guard: no ENV_COMMON_MB, no horn -- the honk is over at once, so
	// the next waits its 3 s as after a real one.
	if (!s->horn) trEndHonk(s);
}

// IsPlayerCarInRange (0x800A5058): the player within 20 m and ahead of the
// car, in the 40-degree cone either side of the way it is moving (flat), and
// not driving the same way it is, within 20 degrees. A car at rest has no
// way it is moving, and never honks. PORT: retail widens the cone to 115
// degrees and drops the second test while TheRaceParameters +0 or an r13
// tweak flag is set (the race kinds EAXSound gives game mode 2); both are
// clear in free roam, where the port's traffic drives.
static bool trPlayerInRange(const TrafficSnd* s)
{
	if (!(trDist(s_trPl.pos, s->in.pos) <= 20.0f)) return false;
	float d[2] = { s_trPl.pos[0] - s->in.pos[0], s_trPl.pos[1] - s->in.pos[1] };
	float v[2] = { s->in.vel[0], s->in.vel[1] };
	const float dl = sqrtf(d[0] * d[0] + d[1] * d[1]), vl = sqrtf(v[0] * v[0] + v[1] * v[1]);
	if (dl <= 0.0f || vl <= 0.0f) return false;
	if ((d[0] * v[0] + d[1] * v[1]) / (dl * vl) < cosf(40.0f * 0.017453292f)) return false;
	const float* f = s_trPl.fwd;
	const float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	if (fl > 0.0f && (f[0] * v[0] + f[1] * v[1]) / (fl * vl) > cosf(20.0f * 0.017453292f))
		return false;
	return true;
}

// CARSFX_TrafficHorn::UpdateParams (0x800A49E0). A car on the road, 3 s
// past its last honk and its last lost draw, with the player coming at it
// over 15 m/s (IsPlayerGoingFastEnough 0x800A4014: Car +0x43C) and in range,
// honks three times in ten -- or always, when no traffic car has for 5 s --
// one of samples 5..7 for 1.2..4 s; then the honk fades out over 150 ms
// (EndCarHonk 0x800A4C68). There is no other way in: the AI's request
// (Car::HonkAndOrBlinkHeadlights, from UpdateHeadlightsAndHorn) is dormant
// in retail, and StartHonkHorn, which sets +0xC4, is an empty stub on the
// GameCube (0x800A4C2C) and the PC (0x4022C0) alike.
static void trHornUpdateParams(TrafficSnd* s, float dt)
{
	if (s->car < 0) return;
	lineUpdate(&s->hornFade, dt);
	const float now = s_trClock;
	if (!s->horn) {
		if (s->in.live && s->hornEnd + 3.0f < now && s->hornTry + 3.0f < now && s->canHonk &&
		    s_trPl.vfwd > 15.0f && trPlayerInRange(s)) {
			if (difficulty_random(s_rng, 1.0f) < 0.3f || s_trLastHonk + 5.0f < now) {
				const int n = (int)rideinfo_random(s_rng, 3);
				s->honking = true;
				s->hornDur = difficulty_random(s_rng, 2.8f) + 1.2f;
				trBeginHonk(s, n + 5);
				if (s_trLogs < 60) {
					s_trLogs++;
					sfxLog("traffic %d (car %d): honk, sample %d for %.1f s, %.1f m off, player %.1f m/s%s",
					       (int)(s - s_tr), s->car, n + 5, s->hornDur, trDist(s_trPl.pos, s->in.pos),
					       s_trPl.vfwd, s->horn ? "" : " -- no ENV_COMMON_MB");
				}
			} else {
				s->hornTry = now;
			}
		}
	} else if ((s->hornStart + s->hornDur <= now || !s->honking) && !s->hornHeld && !s->hornFading) {
		s->hornFading = true;
		lineInit(&s->hornFade, 1.0f, 0.0f, 150, 0);
	}
	if (s->hornFading && s->hornFade.done) trEndHonk(s);
}

// CSIS_UpdateHOnk (0x800A4EDC), its ProcessUpdate: the map's level (output
// 1) through the fade, its azimuth, and the Doppler pitch: GenDopplerFactor
// (eDOPPLER_PARAMS 0) of the car's 3DTrafficPos (#503).
static void trHornProcessUpdate(TrafficSnd* s, uint32_t id)
{
	if (!s->horn) return;
	const float doppler = mixmapDoppler(MIXMAP_COPY_ID(MIXMAP_CTL_3DTRAFFICPOS, (int)(s - s_tr)),
		MIXMAP_DOPPLER_TRAFFICHORN);
	s->hornData[2] = clampi((int32_t)(doppler * 4096.0f), 0, 0x1fa0);
	s->hornData[1] = clampi((int32_t)((float)mixmapQ15(id, 1) * s->hornFade.cur), 0, 0x7fff);
	s->hornData[3] = clampi(mixmapAzim(id, 1), 0, 0xffff);
	aemsSetMemberData(s->horn, s->hornData);
}

// CARSFX_TrafficWoosh::UpdateParams (0x800A5508): the player within 5 m of
// a car on the road, going by it at over 15 m/s, at least 3 s after this
// car's last: an MB_Stitch_Whoosh stitch (colsfx.c type 1) from 9 on, a row
// of six for each third of (15..60 m/s), the next of the row in turn, and
// output 2 set for the frame (the map's envelope 0). PORT: a stitch the pool
// or the bank could not give is no woosh -- retail's wrapper always plays.
static void trWooshUpdateParams(TrafficSnd* s)
{
	if (s->car < 0 || s->wsh || !s->in.live || !(trDist(s_trPl.pos, s->in.pos) < 5.0f)) return;
	const float rel = trRelSpeed(s);
	if (!(rel > 15.0f) || !(s->wshLast + 3.0f <= s_trClock)) return;
	float f = fminf(fmaxf((rel - 15.0f) / 45.0f, 0.0f), 1.0f);
	s_trWooshN %= 6;
	const int n = s_trWooshN++;
	f = fminf(fmaxf(f, 0.0f), 0.99f);
	const float g = fminf(fmaxf(f * 127.0f, 0.0f), 127.0f);
	const int idx = (int)(g * 0.0078125f * 3.0f) * 6 + n + 9;
	memset(s->wshP, 0, sizeof(s->wshP));
	s->wshP[0] = idx;
	s->wsh = colsfxStitchPlay(1, idx, s->wshP);
	const int c = (int)(s - s_tr);
	int32_t* o = mixmapObjOut(MIXMAP_COPY_ID(MIXMAP_SFX_TRAFFICWOOSH, c));
	if (o) o[2] = 1;
	s->wshLast = s_trClock;
	if (s_trLogs < 60) {
		s_trLogs++;
		sfxLog("traffic %d (car %d): woosh, %.1f m/s by -> stitch %d %s", c, s->car, rel, idx,
		       s->wsh ? "playing" : "(none)");
	}
}

// ProcessUpdate (0x800A5704): output 2 back to 0; the stitch at the map's
// level and azimuth (output 2), unit pitch, held only while the player is
// still going by fast, it plays and the game is not paused.
static void trWooshProcessUpdate(TrafficSnd* s, uint32_t id)
{
	int32_t* o = mixmapObjOut(id);
	if (o) o[2] = 0;
	if (!s->wsh) return;
	const int32_t p[5] = { 0, mixmapQ15(id, 2), 0x1000, mixmapAzim(id, 2), 0 };
	colsfxStitchUpdate(s->wsh, p);
	if (trRelSpeed(s) > 15.0f && colsfxStitchPlaying(s->wsh) && !s_trPl.paused) return;
	colsfxStitchFree(s->wsh);
	s->wsh = NULL;
}

// CARSFX_TrafficEngine::ProcessUpdate (0x800A456C): output 3 the effects
// slider (the map scales both envelopes by it); then, the car on the road,
// the map's level and azimuth (output 0), its speed in mph over 140 on
// [0, 1023], and the Doppler pitch, GenDopplerFactor (eDOPPLER_PARAMS 1) of
// the car's 3DTrafficPos (#503); off the road, silent.
static void trEngineProcessUpdate(TrafficSnd* s, uint32_t id)
{
	int32_t* o = mixmapObjOut(id);
	if (o) o[3] = mixmapMasterVolume(4);
	if (s->car < 0 || !s->eng) return;
	int32_t* d = s->engData;
	if (s->in.live) {
		const float mph = fminf(fmaxf(s->in.speed * 2.237f / 140.0f * 1024.0f, 0.0f), 1024.0f);
		const float doppler = mixmapDoppler(
			MIXMAP_COPY_ID(MIXMAP_CTL_3DTRAFFICPOS, (int)(s - s_tr)), MIXMAP_DOPPLER_TRAFFICENGINE);
		d[1] = clampi(mixmapQ15(id, 0), 0, 0x7fff);
		d[3] = clampi(mixmapAzim(id, 0), 0, 0xffff);
		d[4] = clampi((int32_t)mph, 0, 0x3ff);
		d[2] = clampi((int32_t)(doppler * 16383.0f - 16383.0f), -0x3fff, 0x3fff);
	} else {
		d[1] = 0;
		d[3] = 0;
	}
	aemsSetMemberData(s->eng, d);
}

// The state's Attach: each SFX's NewAttachment. The engine (0x800A43E0,
// InitEngine 0x800A4290): the other of the two variants, an FX_TRAFFIC
// instance of it. The stereo (CARSFX_Stereo::NewAttachment 0x800A82E4): a
// traffic car's plays one time in three, never in car types 0x1F and 0x22,
// and its track is drawn again.
static void trAttach(TrafficSnd* s, int k, const CarSfxTrafficCar* car)
{
	s->car = k;
	s->in = *car;
	s_trVariant = (s_trVariant + 1) % 2;
	s->variant = (uint8_t)s_trVariant;
	if (!s->eng && s_trafBank) {   // port guard: no bank, nothing to play
		memset(s->engData, 0, sizeof(s->engData));
		s->engData[0] = s->variant > 0xf ? 0xf : s->variant;
		s->eng = aemsCreate(aemsClass("FX_TRAFFIC"), s->engData);
	}
	s->st.enabled = car->carType != 0x1f && car->carType != 0x22 && rideinfo_random(s_rng, 3) == 0;
	if (!s_envBank) s->st.enabled = false;   // port guard: no ENV_COMMON_MB
	s->st.track = (uint8_t)(8 + rideinfo_random(s_rng, 2));
	if (s_trLogs < 60) {
		s_trLogs++;
		sfxLog("traffic %d: car %d (type %d) attached, engine variant %d%s%s", (int)(s - s_tr), k,
		       car->carType, (int)s->variant, s->eng ? "" : " (no TRAFFIC_MB)",
		       s->st.enabled ? ", stereo on" : "");
	}
}

// The state's Detach: the engine let go (Detach 0x800A4428), the woosh's
// stitch (0x800A54B0), the horn ended and the stereo let go.
static void trDetach(TrafficSnd* s)
{
	letGo(&s->eng);
	colsfxStitchFree(s->wsh);
	s->wsh = NULL;
	if (s->horn) trEndHonk(s);
	letGo(&s->st.inst);
	s->car = -1;
}

void carsfxTrafficPreMix(const CarSfxTrafficCar* cars, int n, const CarSfxTrafficPlayer* pl, float dt)
{
	if (!s_trOn) return;
	if (!cars) n = 0;
	s_trPl = *pl;
	// DisconnectCar: a car gone from the world (the traffic closed, or a
	// race began) leaves its state.
	for (int c = 0; c < MIXMAP_TRAFFIC_COPIES; c++) {
		TrafficSnd* s = &s_tr[c];
		if (s->car >= 0 && (s->car >= n || !cars[s->car].used)) trDetach(s);
	}
	// CSTATEMGR_TrafficCar::UpdateParams: each connected car within 70 m of
	// the closest player car keeps its state or takes the first free one; a
	// car out of it lets its state go.
	for (int k = 0; k < n; k++) {
		if (!cars[k].used) continue;
		const bool in = trDist(pl->pos, cars[k].pos) < TR_SND_RADIUS;
		TrafficSnd* s = NULL;
		for (int c = 0; c < MIXMAP_TRAFFIC_COPIES && !s; c++)
			if (s_tr[c].car == k) s = &s_tr[c];
		if (s) {
			if (!in) trDetach(s);
		} else if (in) {
			for (int c = 0; c < MIXMAP_TRAFFIC_COPIES && !s; c++)
				if (s_tr[c].car < 0) s = &s_tr[c];
			if (s) trAttach(s, k, &cars[k]);
		}
	}
	// CSTATEMGR_Base::UpdateParams: each state's clock, its 3DTrafficPos on
	// the car, the woosh's and the horn's UpdateParams, then the horn's
	// UpdateMixerOutputs (0x800A4CAC): output 1, a honk sounding.
	if (!pl->paused) s_trClock += dt;
	for (int c = 0; c < MIXMAP_TRAFFIC_COPIES; c++) {
		TrafficSnd* s = &s_tr[c];
		if (s->car >= 0) {
			s->in = cars[s->car];
			mixmapSetObjPos(MIXMAP_COPY_ID(MIXMAP_CTL_3DTRAFFICPOS, c), s->in.pos);
		}
		trWooshUpdateParams(s);
		trHornUpdateParams(s, dt);
		int32_t* o = mixmapObjOut(MIXMAP_COPY_ID(MIXMAP_SFX_TRAFFICHORN, c));
		if (o) o[1] = s->horn != NULL;
	}
}

void carsfxTrafficUpdate(float dt)
{
	if (!s_trOn) return;
	// Each state's SFX in ID order: the engine (0), the woosh (1), the horn
	// (2) and the stereo (3).
	for (int c = 0; c < MIXMAP_TRAFFIC_COPIES; c++) {
		TrafficSnd* s = &s_tr[c];
		trEngineProcessUpdate(s, MIXMAP_COPY_ID(MIXMAP_SFX_TRAFFICENGINE, c));
		trWooshProcessUpdate(s, MIXMAP_COPY_ID(MIXMAP_SFX_TRAFFICWOOSH, c));
		trHornProcessUpdate(s, MIXMAP_COPY_ID(MIXMAP_SFX_TRAFFICHORN, c));
		char who[16];
		snprintf(who, sizeof(who), "traffic %d ", c);
		stereoUpdate(&s->st, MIXMAP_COPY_ID(MIXMAP_SFX_TRAFFICSTEREO, c), s->car >= 0, who);
	}
	// Debug: every 5 s, the copies holding a car.
	if ((s_trReportIn -= dt) <= 0.0f) {
		s_trReportIn = 5.0f;
		for (int c = 0; c < MIXMAP_TRAFFIC_COPIES; c++) {
			const TrafficSnd* s = &s_tr[c];
			if (s->car < 0) continue;
			const uint32_t id = MIXMAP_COPY_ID(MIXMAP_SFX_TRAFFICENGINE, c);
			sfxLog("traffic %d: car %d %s %.0f m off, %.1f m/s; engine q15 %d azim %d spd %d%s; "
			       "horn q15 %d%s", c, s->car, s->in.live ? "on the road" : "off", trDist(s_trPl.pos, s->in.pos),
			       s->in.speed, (int)mixmapQ15(id, 0), (int)mixmapAzim(id, 0), (int)s->engData[4],
			       s->eng ? "" : " (none)", (int)mixmapQ15(MIXMAP_COPY_ID(MIXMAP_SFX_TRAFFICHORN, c), 1),
			       s->horn ? " honking" : "");
		}
	}
}

static void trShutdown(void)
{
	for (int c = 0; c < MIXMAP_TRAFFIC_COPIES; c++) trDetach(&s_tr[c]);
	s_trOn = false;
	if (s_trafBank) aemsUnload(s_trafBank);
	s_trafBank = NULL;
}

// ---------------------------------------------------------------- frontend purge

static AemsBank* s_fePurgeBank;
static AemsInst  s_fePurge;
static int32_t   s_fePurgeData[4];
static int32_t   fePurgeLevel(void);

bool carsfxFePurgeOpen(const char* audioDir, int nosUpgrade)
{
	if (s_fePurgeBank || s_nos.bank) return true;   // the world's still up: its bank
	static const char* const kNosBank[2] = { "Nitrous_00_MB", "Nitrous_01_MB" };
	const int which = (nosUpgrade == 2 || nosUpgrade == 3) ? 1 : 0;
	char path[512];
	snprintf(path, sizeof(path), "%sNOS/%s.aem", audioDir, kNosBank[which]);
	s_fePurgeBank = aemsLoad(path, AUDIO_BUS_SFX);
	sfxLog(s_fePurgeBank ? "fe purge: nitrous upgrade %d -> %s"
	                     : "fe purge: nitrous upgrade %d -> %s: not loaded -- the purge silent",
	       nosUpgrade, kNosBank[which]);
	return s_fePurgeBank != NULL;
}

void carsfxFePurgePlay(void)
{
	letGo(&s_fePurge);
	for (int i = 0; i < 4; i++) s_fePurgeData[i] = 0;
	if (!s_fePurgeBank && !s_nos.bank) {
		sfxLog("fe purge: PlayPurge -- no NOS bank, silent");
		return;
	}
	s_fePurge = aemsCreate(aemsClass("FX_PURGE"), s_fePurgeData);
	sfxLog("fe purge: PlayPurge (SND: FEPurge) -> %s, level q%d", s_fePurge ? "FX_PURGE" : "no instance",
	       (int)fePurgeLevel());
}

void carsfxFePurgeStop(void)
{
	if (!s_fePurge) return;
	s_fePurgeData[2] = 1;
	aemsSetMemberData(s_fePurge, s_fePurgeData);
	sfxLog("fe purge: StopPurge");
}

// The FE object's GetQ15Output(5) (#517), as the port plays the frontend: its
// menu clicks (EAXFrontEnd::Play, output 1) go out at full scale, so the
// purge is output 5 under output 1. Both are master 9 of the Main state
// (SFX 0x40000060: -14 dB, then slot 1 -0.5 dB, slot 5 -3.25 dB, the same in
// all four MAPOUTPUT files), so the purge sits 2.75 dB under a click -- Q15
// 23879 -- whether or not the world's map is up.
static int32_t fePurgeLevel(void)
{
	if (mixmapReady()) {
		const int32_t click = mixmapQ15(MIXMAP_SFX_FRONTEND, 1);
		const int32_t purge = mixmapQ15(MIXMAP_SFX_FRONTEND, 5);
		if (click > 0) {
			const int32_t v = (int32_t)((int64_t)purge * 0x7fff / click);
			return v > 0x7fff ? 0x7fff : v;
		}
	}
	return mixmapQ15FromDb(-325 + 50);
}

void carsfxFePurgeUpdate(void)
{
	if (!s_fePurge) return;
	s_fePurgeData[1] = 0;
	s_fePurgeData[3] = 0x1000;
	s_fePurgeData[0] = fePurgeLevel();
	aemsSetMemberData(s_fePurge, s_fePurgeData);
	if (aemsRefCount(s_fePurge) < 2) {
		letGo(&s_fePurge);
		sfxLog("fe purge: FX_PURGE's graphs are done, let go");
	}
}

void carsfxFePurgeClose(void)
{
	letGo(&s_fePurge);
	if (s_fePurgeBank) aemsUnload(s_fePurgeBank);
	s_fePurgeBank = NULL;
}
