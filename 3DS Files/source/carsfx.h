// The cars' sound effects: retail's CARSFX controllers (EAXSound's per-car
// SFX objects, Speed.elf 0x80097DEC..0x800A4CC4), which watch the car every
// frame and feed the AEMS graphs (aems.h) their member data. TASKS.md #298.
// The player's engine, shift, backfire, turbo, nitrous, skids, road, wind
// and weather wind noise, landings and bottom-outs, tunnels and their
// drive-by wooshes, the roadside and passing-car wooshes (carsfxWoosh*), the
// opponents' engines, turbos, nitrous, skids and stereos (carsfxAi*, below),
// and the traffic's engines, wooshes, horns and stereos (carsfxTraffic*,
// below), so far:
//
//   CARSFX_PlayerEngine  ctor 0x8009FDA0, RegisterData 0x800A00AC,
//                        InitializeEngine 0x800A0208,
//                        SetGinsuParamsSingleMix 0x800A03D0,
//                        SetGinsuParamsDualMix 0x800A05F0,
//                        SetEngineParams 0x800A0950,
//                        SetupLoadData 0x800AF06C
//   EAXCar::CarPreSetup  0x8007FBB8 (SPEED2.EXE's twin picks the dual Ginsu)
//   CARSFX_Shift         see carsfx.c (FX_SHIFTING_01 and CAR_SWTN)
//   CARSFX_SparkChatter  see carsfx.c (CAR_Sputter); its message from
//                        Car::ChangeGear 0x80220B64 and
//                        TriggerCarTerrainEffects (carsim_backfire)
//   CARSFX_Turbo         see carsfx.c (FX_TURBO_01)
//   CARSFX_Nitrous       SetupSFX 0x8009A768, SetupLoadData 0x800AEA2C,
//                        Play 0x8009A804, Stop 0x8009AA18, PlayPurge 0x8009AA68,
//                        StopPurge 0x8009AB98, UpdateParams 0x8009ACF0,
//                        ProcessUpdate 0x8009AF78
//   CARSFX_Skids         SetupSFX 0x8009EB64, InitSFX 0x8009EC04,
//                        InitSkids 0x8009EC7C, UpdateParams 0x8009EE40,
//                        ProcessUpdate 0x8009EFD4, SetupLoadData 0x800AEEFC
//   SFXCTL_Skid          SetupCTL 0x8008E72C, InitCTL 0x8008E814,
//                        UpdateParams 0x8008E924, GenerateSkidNoise 0x8008E9F4,
//                        GetSkidMag 0x8008F444
//   SFXCTL_Wheel         GenerateWheelPosition 0x8008F7BC,
//                        UpdateTireParams 0x8008F9D4,
//                        GenerateTerrainTypes 0x8008FB08, GetTerrainType 0x8008FBC8
//   CARSFX_RoadNoise     InitSFX 0x8009BD8C, ProcessUpdate 0x8009C224,
//                        PlayTransition 0x8009CB30, GenerateRoadNoise 0x8009D0F0,
//                        GenTerrainType 0x8009D548, Play 0x8009D5D4,
//                        SetupLoadData 0x800AEB50
//   CARSFX_WindNoise     InitSFX 0x8009DBAC, UpdateParams 0x8009DD10,
//                        UpdateCSISParams 0x8009E2A4
//   CARSFX_BottomOut     LandJumpPlay 0x800A5B84, BottomOutPlay 0x800A5E10,
//                        UpdateParams 0x800A5FE8, ProcessUpdate 0x800A6530;
//                        its message from Car::TriggerCarTerrainEffects
//                        0x80221D38 (carsim_bottom_out)
//   SFXCTL_Tunnel        UpdateDriveBySnds 0x80095D3C, UpdateParams 0x80096114,
//                        UpdateMixerOutputs 0x800963D0 (not its reverb half)
//   CARSFX_DriveBy       UpdateParams 0x800A20B0, ProcessUpdate 0x800A23B0
//   SFXObj_Woosh         UpdateParams 0x800A2DEC, ProcessUpdate 0x800A2EB4,
//                        NewAttachment 0x800A302C; CSTATEMGR_DriveBy
//                        GetFreeState 0x800AAB18; its messages from
//                        EAXTunerCar::ProcessSoundSphere 0x8007F404 and
//                        EAXAITunerCar::UpdatAIDriveBy 0x8007EF6C
//   Car::IsBlowOff       0x802242A4
//   SFX_Base::AdjustPitch 0x80097EC8
//
// The engine's revs, torque and levels come from its SFXCTL controllers
// (enginectl.h). Every SFX's volume and azimuth come off the mix map
// (mixmap.h), which carsfxSetup loads. Each frame the caller runs
// carsfxPreMix (the controllers), mixmapUpdate, then carsfxUpdate (the SFX).
#pragma once

#include "enginectl.h"

#include <stdbool.h>
#include <stdint.h>

// What CarPreSetup and the SFX setups read off the car, once.
typedef struct {
	int   nosUpgrade;   // GetUpgradeLevel(NITROUS)
	int   carType;      // DriverInfo +8: the car type, GetCarTypeMapping's key
	int   engLevel;     // CSTATE +0x90: 3 / 2 / 1 with perf part 0x18 / 0x13 / 0xF, else 0
	int   transLevel;   // CSTATE +0xA0: GetUpgradeLevel(TRANSMISSION), 0..3
	int   turboLevel;   // CARSFX_Turbo +0xAC: GetUpgradeLevel(TURBO)
	float boost[9];     // CarPhysicsInfo +0x224: the built block's boost curve
	float idle, redline, limiter;   // the car's EngineInfo (CarPhysicsInfo +0x1F0)
	bool  drift;        // DAT_803FCA84 || g_tweakIsDriftRace: a drift race
	int   mixMap;       // GetMixMap (0x8007AB18): 0 MAPOUTPUT, 1 DFT, 2 DRG, 3 URL
	uint32_t* rng;      // bRandom's state
} CarSfxSetup;

typedef struct {
	int   nosLevel;   // CarState::IsNitrousOn: the level while the bottle burns, 0 shut
	// CARSFX_Hydraulics' (#313): Car_GetHydraulicState per corner (0 down, 1
	// up, 2 rising, 3 lowering) and CarState +0x3CC, the corners a bounce
	// kicked this frame.
	int   hydState[4];
	bool  hydFired[4];
	float throttle;   // CarDriver +0x1F0 (SetThrottle): the pedal the car is given
	float speed;      // CarState +0x400 (SetVelocity): |velocity|, m/s
	bool  paused;     // EAXSound +0x1C: the pause menu is up (EnterPauseMenu)
	EngineCtlIn eng;  // what the engine's controllers read

	// What the skids read: retail's per-wheel CarState arrays (UpdateCar), the
	// wheels numbered as it does, 0 front left, 1 front right, 2 rear right,
	// 3 rear left.
	struct {
		bool  onGround;   // Car +0x388 + i
		float gripUse;    // CarState +0x38C [i]: Wheel +0x16C, 0..1
		float slip[2];    // CarState +0x36C [i]: Wheel +0x100 / +0x104
		int   terrain;    // Car +0x38C [i]: eTerrainType (1 ROAD, 8 DIRT, 0xF GRASS, ...)
	} wheel[4];
	float pos[3];        // Car +0x58
	float vel[3];        // Car +0x78
	float fwd[3];        // Car +0x88, its matrix's row 0 ...
	float left[3];       // ... and row 1
	float vfwd;          // CarState +0x3FC: the speed along the heading
	float yaw;           // CarState::GetYaw, rad
	float steerDeg;      // CarState +0x3DE in degrees, -180 .. 180
	float brake, ebrake; // CarDriver +0x1F8 (SetBrake) / +0x1F4 (SetEBrake)

	// What CARSFX_BottomOut reads.
	int   bottomOut;     // a pending SFXMessage 5: CSTATE +0xD4, 0..127; -1 none
	// What CARSFX_SparkChatter reads: a backfire this step (carsim_backfire),
	// SFXMessage 9.
	bool  backfire;
	float upZ;           // Car +0xB0: the matrix's up row, its world z
	float suspRatio[4];  // each spring's compression over its bump stop
	                     // (SuspensionInfo [5]), JumpLandingIntensity's input
	bool  physicsMover;  // Car +0x28 == 1: on the PhysicsMover, not the Mellovator

	// What SFXCTL_Tunnel reads: TheTrackPathManager's zones (NULL, none).
	const struct TrackPaths* paths;
	// What Car::TriggerEvents set off this step (emevents.h): the triggers
	// round the point 0.15 s ahead of the car, which ProcessSoundSphere turns
	// into the roadside wooshes.
	const struct EmEvent* const* events;
	int   nEvents;

	// What CARSFX_Rain reads (TASKS.md #315): view 1's Rain +0x1F0 (rain.h's
	// rain_intensity), IsWorldDataStreaming(0), and QueueWeatherStream's gate:
	// the race coordinator not in CameraFlyby, PreRace or PostRace (states 2,
	// 3, 7) -- the port's countdown and a finished race.
	float rain;
	bool  worldStreaming;
	bool  weatherStreamOk;
} CarSfxIn;

// SetupSFX + SetupLoadData for the player's car. Nitrous: levels 2 and 3 load
// Nitrous_01_MB, anything else Nitrous_00_MB. The engine: the car type's
// AEMS CAR bank and Ginsu recordings, by its engine and gearbox levels. The
// shift: the gearbox's GEAR bank and the engine's SWTN_CAR bank. The turbo:
// the turbo level's TURBO bank, silent without a boost curve. The skids: a
// car's or a truck's SKIDS bank (the drift ones only in a drift race). Road
// and wind noise: IG_GLOBAL's ROADNOISE_00_MB and WIND_00_MB. The
// opponents' stereos: IG_GLOBAL's ENV_COMMON_MB (SFX_Common's).
// `audioDir` is the game's audio/ directory, with its trailing slash. A
// missing bank leaves that sound silent, nothing else.
void carsfxSetup(const char* audioDir, const CarSfxSetup* s);
// The PlayerCar state's controllers, before mixmapUpdate.
void carsfxPreMix(const CarSfxIn* in, float dt);
// UpdateParams then ProcessUpdate, once a frame while the world is up --
// under the pause screen and the Cingular windows too, so a sound already
// going winds down rather than hangs.
void carsfxUpdate(const CarSfxIn* in, float dt);
// Lets go of every instance and unloads the banks (leaving the world), the
// opponents' too.
void carsfxShutdown(void);
// The DriveBy state (6): CSTATEMGR_DriveBy's four one-shots, an SFXObj_Woosh
// each, set off by the sound spheres carsfxPreMix is handed and by an
// opponent passing close (carsfxAiPreMix). Their UpdateParams after the
// Collision states' (colsfxPreMix), before mixmapUpdate; their ProcessUpdate
// after colsfxUpdate.
void carsfxWooshPreMix(void);
void carsfxWooshUpdate(void);

// The opponents: CSTATEMGR_AICar (EnterWorld 0x800AA0BC,
// ResolveEngineBankLoading 0x800AA3B4) and each car's AICar state. Its SFX 0
// is a CARSFX_AIEngine (ctor 0x8009F76C, InitializeEngine 0x8009FBE0,
// SetEngineParams 0x8009F92C, SetupLoadData 0x800AF27C), which plays the
// AEMS CAR graph alone (no Ginsu); the race's opponents share at most three
// engine banks, picked by each car type's engine group. Its SFX 2, 3 and 5
// are the player's CARSFX_Turbo, CARSFX_Nitrous and CARSFX_Skids (CreateSFX
// 0x800A913C), on its own mix-map copies and the player's banks -- the
// skids with one voice, the nitrous without a purge the map lets through.
//
// What EAXAITunerCar::CarPreSetup (0x8007ECD4) and the SFX setups read off an
// opponent, once.
typedef struct {
	int   carType;      // DriverInfo +8
	int   engLevel;     // CSTATE +0x90, 0..3
	int   transLevel;   // CSTATE +0xA0, 0..3
	int   turboLevel;   // its DriverInfo's GetUpgradeLevel(TURBO)
	float boost[9];     // its CarPhysicsInfo +0x224
	float idle, redline, limiter;   // its EngineInfo (CarPhysicsInfo +0x1F0)
} CarSfxAiCar;

// An opponent, each frame.
typedef struct {
	EngineCtlAiIn eng;  // what the AICar state's controllers read
	bool  live;         // EAXCar +0x30: it has a Car
	// Its Car and CarState as the player's SFX read them: the pose (its
	// 3DCarPos and drive-by too), the wheels, nosLevel, throttle and speed.
	// The rest is unread.
	CarSfxIn car;
} CarSfxAiIn;

// Sets up `n` opponents (at most ENGINECTL_AI_MAX; copy k is car k) after
// carsfxSetup, which loads the mix map they play through. `drift` and
// `shortTrack` are the race's kind. n 0 tears them down.
void carsfxAiSetup(const char* audioDir, const CarSfxAiCar* cars, int n, bool drift,
	bool shortTrack, uint32_t* rng);
// Their controllers and 3DCarPos positions, before mixmapUpdate (beside
// carsfxPreMix), then their SFX after it (after carsfxUpdate, whose pause
// flag the turbo reads).
void carsfxAiPreMix(const CarSfxAiIn* in, int n, float dt);
void carsfxAiUpdate(const CarSfxAiIn* in, int n, float dt);
void carsfxAiShutdown(void);

// The traffic: CSTATEMGR_TrafficCar (EnterWorld 0x800A9D20, UpdateParams
// 0x800A9E48, ConnectCar 0x800A9FAC, DisconnectCar 0x800A9F80) and its six
// Traffic states, which carsfxSetup makes. A state goes to a traffic car
// within 70 m of the player and leaves it when it drives out of that. Each
// plays the car's
//
//   CARSFX_TrafficEngine  NewAttachment 0x800A43E0, InitEngine 0x800A4290,
//                         ProcessUpdate 0x800A456C (FX_TRAFFIC, TRAFFIC_MB)
//   CARSFX_TrafficWoosh   UpdateParams 0x800A5508, ProcessUpdate 0x800A5704,
//                         GetPlayerSpeedRelativeToUs 0x800A5810
//   CARSFX_TrafficHorn    UpdateParams 0x800A49E0, CSIS_BeginHonk 0x800A4CC4,
//                         CSIS_EndHonk 0x800A4E44, CSIS_UpdateHOnk 0x800A4EDC,
//                         IsPlayerCarInRange 0x800A5058, EndCarHonk 0x800A4C68
//                         (ENV_STATIC, ENV_COMMON_MB); IsPlayerGoingFastEnough
//                         0x800A4014
//   CARSFX_Stereo         NewAttachment 0x800A82E4
//
// through the map's Traffic copies, positioned by SFXCTL_3DTrafficPos.
//
// A traffic car, each frame (retail's Car fields).
typedef struct {
	bool  used;      // connected: the car exists (ConnectCar)
	bool  live;      // +0x548: on the road and drawn
	float pos[3];    // +0x58
	float vel[3];    // +0x68
	float speed;     // +0x440: |velocity|, m/s
	int   carType;   // +0x10's: types 0x1F and 0x22 never get a stereo
} CarSfxTrafficCar;

// The player's car as they read it (GetClosestPlayerCar).
typedef struct {
	float pos[3];    // +0x58
	float vel[3];    // +0x68
	float fwd[3];    // +0x88, the matrix's forward row
	float vfwd;      // +0x43C: the speed along the heading, m/s
	bool  paused;    // the pause menu is up
} CarSfxTrafficPlayer;

// The manager and each state's UpdateParams, before mixmapUpdate (after
// carsfxAiPreMix); `n` cars, car k the traffic's car k (0 when there is no
// traffic: every state lets go). Then their ProcessUpdate after it (after
// carsfxAiUpdate). carsfxShutdown ends them.
void carsfxTrafficPreMix(const CarSfxTrafficCar* cars, int n, const CarSfxTrafficPlayer* pl, float dt);
void carsfxTrafficUpdate(float dt);

// ---------------------------------------------------------------- frontend purge
// EAXFrontEnd::PlayPurge / StopPurge / Update (SpeedPS 0x8007DE44,
// 0x8007DF6C, 0x8007E194): the specialties shop's Nitrous Purge preview,
// Y held on the purge list (IcePartsBrowser 0xC519BFC0 / 0xD871B0A4).
// PlayPurge releases any purge still held and makes a new FX_PURGE, its four
// fields 0; StopPurge sets the stop flag (+0xC, field 2); Update gives it
// volume (field 0, the FE object's GetQ15Output(5)), azimuth 0 and pitch
// 0x1000 each frame and lets go once its graphs are done (refcount < 2).
// The class is the NOS banks' (CARSFX_Nitrous::SetupLoadData's g_NOS pick:
// levels 2 and 3 Nitrous_01_MB, else Nitrous_00_MB), which the frontend
// holds for the car in the garage; carsfxFePurgeOpen loads it.
// PORT: the port plays the frontend's clicks (output 1) at full scale, so the
// purge plays at output 5 over output 1 (2.75 dB under a click, #517).
bool carsfxFePurgeOpen(const char* audioDir, int nosUpgrade);
void carsfxFePurgePlay(void);
void carsfxFePurgeStop(void);
void carsfxFePurgeUpdate(void);
void carsfxFePurgeClose(void);
