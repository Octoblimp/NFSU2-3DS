// NFSU2-3DS: retail's mix map (NFSMixMap, Speed.elf 0x800B1FB0 on).
//
// Every in-game sound's volume and azimuth come out of a data file,
// SOUND/MIXMAPS/MAPOUTPUT.mxb, not out of the code that plays it. The file
// describes, per main map state (0 Main, 1 PlayerCar, ...), a little signal
// network: data procs shape the SFX controllers' outputs (the volume sliders,
// the speech flag) through NFSMixShape's curves, MixCtls turn those into dB,
// event MixCtls are envelopes a trigger starts (the pause, a line of speech),
// 3D MixCtls attenuate and pan by the camera's distance and angle to an object,
// and sub and master channels sum all of it in hundredths of a dB. A master
// channel ends in output words, Q15 volume | azimuth << 16, which its SFX
// object reads through SFX_Base::GetQ15Output / GetAzimOutput.
//
// Ported: the whole runtime, run over the PC file. Main state and PlayerCar
// state each get the one copy the single-player game creates, the AICar state
// the five a race's opponents can need, the Collision state its five
// (CSTATEMGR_Collision's), the DriveBy state its four (CSTATEMGR_DriveBy's),
// the Traffic state its six (CSTATEMGR_TrafficCar's), and the world's
// Fountain, Plane and Train states their four, three and six (#509,
// envsfx.c). The SFX and SFXCTL
// objects the map reads and feeds are the ported ones only: SFXCTL_MasterVol,
// the speech module, SFXObj_Music (the EA Trax stream's level),
// SFXCTL_3DCarPos with its SndCamera, the player engine's controllers
// (enginectl.h) with CARSFX_PlayerEngine, CARSFX_Shift, CARSFX_Turbo and
// CARSFX_Nitrous, the skids' SFXCTL_Skid, SFXCTL_3DRightWheelPos /
// 3DLeftWheelPos and CARSFX_Skids, CARSFX_RoadNoise, CARSFX_WindNoise,
// CARSFX_BottomOut, SFXCTL_Tunnel with its 3DTunnelPos and CARSFX_DriveBy
// (carsfx.c), an opponent's 3DCarPos, CARSFX_AIEngine, CARSFX_Turbo,
// CARSFX_Nitrous and CARSFX_Skids, a Collision copy's
// SFXObj_Collision, 3DColPos and 3DScrapePos (colsfx.c), a DriveBy copy's
// SFXObj_Woosh and 3DWooshPos (carsfx.c), and a Traffic copy's
// SFXCTL_3DTrafficPos, CARSFX_TrafficEngine, CARSFX_TrafficWoosh,
// CARSFX_TrafficHorn and CARSFX_Stereo (carsfx.c), and the world's
// SFXObj_Fountain / Plane / Train with their 3DObjPos controllers (envsfx.c).
// Anything else
// the file names reads as a zero (mixmap.c says why), and a master channel
// whose SFX is not ported is never run.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Loads and builds the map. False (and every reader below then a no-op that
// says so) if the file is missing or malformed.
bool mixmapInit(const char* mxbPath);
void mixmapShutdown(void);
bool mixmapReady(void);

// What the ported SFX controllers read from the game each frame.
typedef struct {
	float camEye[3];   // SndCamera: the camera's position and the point it looks at
	float camLook[3];
	float carPos[3];   // the player car's position and forward axis
	float carFwd[3];
	int   povType;     // CARDB_POV_* of the view in use
	int   zone;        // the player's zone (Player +0x58, main.c): 6 the crash camera
	bool  paused;      // the pause menu is up
} MixMapIn;

// One frame: the ported controllers' UpdateParams, then
// NFSMixMap::ProcessMixMap(dt), in EAXSound::Update's order. `dt` is real
// time, and keeps running while paused (the pause's own fade needs it).
void mixmapUpdate(const MixMapIn* in, float dt);

// The SFX objects the map feeds, by their retail ID.
#define MIXMAP_SFX_MUSIC        0x40000000u   // SFXObj_Music, Main state
#define MIXMAP_SFX_COMMON       0x40000040u   // SFX_Common, Main state
#define MIXMAP_SFX_FRONTEND     0x40000060u   // EAXFrontEnd's FE object, Main state (#517)
#define MIXMAP_SFX_PLAYERENGINE 0x40010000u   // CARSFX_PlayerEngine, PlayerCar state
#define MIXMAP_SFX_SHIFT        0x40010010u   // CARSFX_Shift, PlayerCar state
#define MIXMAP_SFX_TURBO        0x40010020u   // CARSFX_Turbo, PlayerCar state
#define MIXMAP_SFX_NITROUS      0x40010030u   // CARSFX_Nitrous, PlayerCar state
#define MIXMAP_SFX_SPARKCHATTER 0x40010040u   // CARSFX_SparkChatter, PlayerCar state
#define MIXMAP_SFX_SKIDS        0x40010050u   // CARSFX_Skids, PlayerCar state
#define MIXMAP_SFX_ROADNOISE    0x40010060u   // CARSFX_RoadNoise, PlayerCar state
#define MIXMAP_SFX_WINDNOISE    0x40010070u   // CARSFX_WindNoise, PlayerCar state
#define MIXMAP_SFX_RAIN         0x40010090u   // CARSFX_Rain, PlayerCar state
#define MIXMAP_SFX_HYDRAULICS   0x40010080u   // CARSFX_Hydraulics, PlayerCar state (#313)
#define MIXMAP_SFX_WINDWEATHER  0x400100a0u   // CARSFX_WindWeather, PlayerCar state
#define MIXMAP_SFX_BOTTOMOUT    0x400100b0u   // CARSFX_BottomOut, PlayerCar state
#define MIXMAP_SFX_DRIVEBY      0x400100c0u   // CARSFX_DriveBy, PlayerCar state
#define MIXMAP_SFX_AIENGINE     0x40020000u   // CARSFX_AIEngine, AICar state (copy 0)
#define MIXMAP_SFX_AISTEREO     0x400200f0u   // CARSFX_Stereo, AICar state (copy 0)
#define MIXMAP_SFX_COLLISION    0x40050000u   // SFXObj_Collision, Collision state (copy 0)
#define MIXMAP_SFX_WOOSH        0x40060000u   // SFXObj_Woosh, DriveBy state (copy 0)
#define MIXMAP_SFX_TRAFFICENGINE 0x40030000u  // CARSFX_TrafficEngine, Traffic state (copy 0)
#define MIXMAP_SFX_TRAFFICWOOSH 0x40030010u   // CARSFX_TrafficWoosh, Traffic state (copy 0)
#define MIXMAP_SFX_TRAFFICHORN  0x40030020u   // CARSFX_TrafficHorn, Traffic state (copy 0)
#define MIXMAP_SFX_TRAFFICSTEREO 0x40030030u  // CARSFX_Stereo, Traffic state (copy 0)
#define MIXMAP_SFX_FOUNTAIN     0x40040000u   // SFXObj_Fountain, Fountain state (copy 0)
#define MIXMAP_SFX_PLANE        0x40070000u   // SFXObj_Plane, Plane state (copy 0)
#define MIXMAP_SFX_TRAIN        0x40080000u   // SFXObj_Train, Train state (copy 0)

// The PlayerCar state's controllers whose outputs the map reads.
#define MIXMAP_CTL_PHYSICS     0x60010000u
#define MIXMAP_CTL_SHIFTING    0x60010030u
#define MIXMAP_CTL_ACCELTRANS  0x60010040u
#define MIXMAP_CTL_ENGINE      0x60010050u
#define MIXMAP_CTL_HYBRIDMOTOR 0x60010060u
#define MIXMAP_CTL_3DCARPOS    0x60010080u
#define MIXMAP_CTL_TUNNEL      0x60010070u
#define MIXMAP_CTL_3DTUNNELPOS 0x600100a0u
#define MIXMAP_CTL_SKID        0x60010020u
#define MIXMAP_CTL_3DRWHEELPOS 0x600100b0u
#define MIXMAP_CTL_3DLWHEELPOS 0x600100c0u
#define MIXMAP_CTL_AI3DCARPOS  0x60020080u   // AICar state (copy 0)
#define MIXMAP_CTL_3DCOLPOS    0x60050000u   // Collision state (copy 0)
#define MIXMAP_CTL_3DSCRAPEPOS 0x60050010u   // Collision state (copy 0)
#define MIXMAP_CTL_3DWOOSHPOS  0x60060000u   // DriveBy state (copy 0)
#define MIXMAP_CTL_3DTRAFFICPOS 0x60030000u  // Traffic state (copy 0)
#define MIXMAP_CTL_3DFOUNTAINPOS 0x60040000u // Fountain state (copy 0)
#define MIXMAP_CTL_3DENVIROPOS 0x60070000u   // Plane state (copy 0): SFXCTL_3DEnviroObjPos
#define MIXMAP_CTL_3DTRAINPOS  0x60080000u   // Train state (copy 0)

// The AICar state's copy `copy` of an object: retail's IDs carry the state's
// type in bits 16..23 and the copy in bits 11..15. A PlayerCar ID in, the
// same object of an opponent out.
#define MIXMAP_AICAR_ID(id, copy) \
	(((uint32_t)(id) & 0xff00ffffu) | 0x00020000u | ((uint32_t)(copy) << 11))
#define MIXMAP_AI_COPIES 5
// The Collision state's copy `copy` of an object (copy 0 in).
#define MIXMAP_COPY_ID(id, copy) ((uint32_t)(id) | ((uint32_t)(copy) << 11))
#define MIXMAP_COL_COPIES 5
// The DriveBy state's (copy 0 in, as above): CSTATEMGR_DriveBy::EnterWorld
// (0x800AAAB8) makes four.
#define MIXMAP_DRIVEBY_COPIES 4
// The Traffic state's: CSTATEMGR_TrafficCar::EnterWorld (0x800A9D20) makes
// one per traffic driver the race has, at most six.
#define MIXMAP_TRAFFIC_COPIES 6
// The world's (#509): CSTATEMGR_Enviro's, CSTATEMGR_Planes' and
// CSTATEMGR_Trains' EnterWorld (0x800AB6B0, 0x800AAD5C, 0x800AB270).
#define MIXMAP_FOUNTAIN_COPIES 4
#define MIXMAP_PLANE_COPIES 3
#define MIXMAP_TRAIN_COPIES 6

// A ported object's outputs (SFXCTL +4 on), which its controller writes and
// the map's data procs read; NULL for an object the port does not have. Valid
// whether or not the map loaded. mixmapInit clears them.
int32_t* mixmapObjOut(uint32_t id);

// Where an SFXCTL_3DObjPos's object is this frame (world x, y, z; the map
// reads x and y) -- what retail hands it as a pointer at InitCTL. The wheel
// positions are SFXCTL_Wheel's (carsfx.c). A no-op for an unported object.
// NULL is AssignPositionVector(NULL): its outputs hold until a position comes.
void mixmapSetObjPos(uint32_t id, const float pos[3]);

// EAXSound::GetMasterVolume (0x8007B23C) with its flag set: the master slider
// times channel `ch`'s (1 speech, 2 game music, 3 menu music, 4 effects,
// 5 engine, 6 tires), Q15.
int32_t mixmapMasterVolume(int ch);

// NFSMixShape::GetCurveOutput (0x800B5AEC) and GetQ15FromHundredthsdB
// (0x800B592C), which the controllers use too.
int32_t mixmapCurveOut(int t, int32_t x, bool dB);
int32_t mixmapQ15FromDb(int32_t x);

// SFX_Base::GetQ15Output (0x800BE8B0) and GetAzimOutput (0x800BE8D8) of the
// object's `slot`: 0 when the slot has no channel. The map not loaded, they
// give 0x7fff and 0 (full volume, straight ahead), the port's old stand-in.
int32_t mixmapQ15(uint32_t sfxId, int slot);
int32_t mixmapAzim(uint32_t sfxId, int slot);

// SFXCTL_3DObjPos::GenDopplerFactor (0x80097C5C, #503) of the 3DObjPos
// controller `ctlId` (MIXMAP_CTL_*, with its copy): the pitch factor its
// object's coming or going makes, 0.8..1.2 (1 standing still), scaled by
// eDOPPLER_PARAMS `which` -- MIXMAP_DOPPLER_*. 0 while paused, as retail;
// 1 without the map or for an unported controller.
#define MIXMAP_DOPPLER_TRAFFICHORN   0
#define MIXMAP_DOPPLER_TRAFFICENGINE 1
#define MIXMAP_DOPPLER_AIENGINE      2
#define MIXMAP_DOPPLER_PLANE         5   // SFXObj_Plane::ProcessUpdate's (#509)
float mixmapDoppler(uint32_t ctlId, int which);
