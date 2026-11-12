#pragma once
#include <3ds.h>
#include <stdbool.h>

// The PC's display settings (TASKS.md #326): the Display and Customize
// pages' values, the six Level of Detail presets, the caps a card sets and
// where the values are kept. The decompiles are tools/re/wip326 (core.c,
// reg.c); the renderer's reads of each value are listed in TASKS.md #326's
// step 1.
//
// Where the settings live
// -----------------------
// SPEED2.EXE keeps them in one 0x78-byte block at 0x870CB0, a u32 per value,
// and g_PerformanceLevel apart at 0x87099C. FUN_005BEA20 loads the block from
// the registry (HKLM\Software\EA Games\Need for Speed Underground 2, one
// REG_DWORD per value, by the names DISPLAY_KEYS lists) and FUN_005BEEA0
// saves it there. They are the machine's, not a profile's: UserProfile has no
// copy.
//
// PORT: a 3DS has no registry, and the user asked for the settings to come
// with the profile ("loaded from profiles/saves"), so each profile keeps them
// in a text file beside its save, <save>.display, a "name=value" line per
// registry value under the registry's own names. The PC's values carry over
// as they are: tools/display_from_registry.ps1 writes the file from the PC's
// key. The stereoscopic 3D settings, which the PC does not have, go in the
// same file under names of their own.

// The block, offset for offset.
typedef struct {
	u32 version;      // +0x00 VERSION, 1
	u32 size;         // +0x04 SIZE, 0x78
	u32 envmap;       // +0x08 g_CarEnvironmentMapEnable: 1 one cube face a frame,
	                  //       2 faces 0 and 4 and one more, 3 all six
	u32 envUpdate;    // +0x0C g_CarEnvironmentMapUpdateData: scenery in the faces
	u32 shadow;       // +0x10 g_CarShadowEnable 0..2
	u32 headlight;    // +0x14 g_CarHeadlightEnable
	u32 carLighting;  // +0x18 g_CarLightingEnable, capped 0 on every card
	u32 carDamage;    // +0x1C g_CarDamageEnable, capped 0 on every card; the
	                  //       port's Car Damage row (#324, options.h)
	u32 crowd;        // +0x20 g_CrowdEnable
	u32 roadRefl;     // +0x24 g_RoadReflectionEnable 0..3
	u32 fog;          // +0x28 g_FogEnable
	u32 motionBlur;   // +0x2C g_MotionBlurEnable
	u32 lightStreaks; // +0x30 g_LightStreaksEnable
	u32 lightGlow;    // +0x34 g_LightGlowEnable
	u32 animTex;      // +0x38 g_AnimatedTextureEnable
	u32 particles;    // +0x3C g_ParticleSystemEnable
	u32 dof;          // +0x40 g_DepthOfFieldEnable
	u32 worldLod;     // +0x44 g_WorldLodLevel 0..3 (FUN_005BE1C0: 3 reads as 2)
	u32 carLod;       // +0x48 g_CarLodLevel 0 Low / 1 High
	u32 overBright;   // +0x4C g_OverBrightEnable
	u32 bleach;       // +0x50 g_BleachByPassEnable
	u32 tinting;      // +0x54 g_TintingEnable
	u32 fsaa;         // +0x58 g_FSAALevel
	u32 horizonFog;   // +0x5C g_HorizonFogEnable
	u32 rain;         // +0x60 g_RainEnable
	u32 u64;          // +0x64 no name, no row: default 1, clamped like the rest
	u32 texFilter;    // +0x68 g_TextureFiltering 0 point / 1 bilinear / 2 anisotropic
	u32 racingRes;    // +0x6C g_RacingResolution, the Resolution row's mode index
	u32 notFirstTime; // +0x70 NotFirstTime: 0 runs the first-run pick
	u32 vsync;        // +0x74 g_VSyncOn
} DisplayBlock;

_Static_assert(sizeof(DisplayBlock) == 0x78, "the display block is 0x78 bytes");

// The block's values as an array, for the caps and the file: value i is the
// u32 at +0x08 + 4 i, the slot the caps table at 0x8764D0 mirrors.
#define DISPLAY_FIRST_VALUE 2 // u32 index of envmap
#define DISPLAY_CAPPED      25 // envmap .. texFilter, the caps FUN_005BE690 reads

typedef struct {
	DisplayBlock b;
	u32 performanceLevel; // g_PerformanceLevel, 0x87099C: Level of Detail 0..5

	// PORT: stereoscopic 3D, which the PC does not have. The hardware slider
	// still sets how much of it shows; these set the most it can be.
	u32 stereo;       // 0 Off: the top screen is drawn once, the slider ignored
	u32 stereoDepth;  // 0..DISPLAY_STEREO_STEPS: the eyes' separation at full slider
	u32 stereoFocus;  // 0..DISPLAY_STEREO_STEPS: how far ahead the screen's plane sits
	u32 stereoHud;    // 0..DISPLAY_STEREO_STEPS: how far the HUD stands out of the screen
	u32 stereoStyle;  // DISPLAY_STEREO_INTO or DISPLAY_STEREO_POP (TASKS.md #365)
} DisplaySettings;

// The 3-D Style row (#365): the picture behind the screen, the car at the
// glass and the street going in, or out of it, the car and the road near it
// standing in front of the glass (stereo.c).
#define DISPLAY_STEREO_INTO 0
#define DISPLAY_STEREO_POP  1

#define DISPLAY_STEREO_STEPS 10
#define DISPLAY_LEVEL_MAX 5

// The settings every draw reads. A profile's load fills it; with no profile it
// holds the first-run pick.
extern DisplaySettings g_display;

// FUN_005B7600: the block's defaults. Everything 0 but VERSION, SIZE, animTex,
// +0x64 and texFilter. The PORT stereo fields take their own defaults.
void display_default(DisplaySettings* d);

// PORT: the stereo fields' defaults alone -- 3-D on, full depth, focus and
// HUD half way.
void display_default_stereo(DisplaySettings* d);

// FUN_005BE870 then the clamp, FUN_005BE9F0: the Level of Detail preset for
// d->performanceLevel. What the LOD slider's act (0x49B740) runs.
void display_apply_level(DisplaySettings* d);

// FUN_005BE690: every value down to this port's cap.
void display_clamp(DisplaySettings* d);

// FUN_005CEC90, the first-run pick: a level, the preset, the clamp. The PC
// takes the level from its card's benchmark; the 3DS has one GPU, so it is
// DISPLAY_FIRST_RUN_LEVEL. Also what Default Settings runs.
#define DISPLAY_FIRST_RUN_LEVEL 3
void display_first_run(DisplaySettings* d);

// The cap of block value `index` (DISPLAY_FIRST_VALUE .. +DISPLAY_CAPPED-1,
// in u32s from the block's start): the highest value this port draws. A
// Customize row exists only when its value's cap is non-zero, as on the PC.
u32 display_cap(int index);
u32* display_value(DisplaySettings* d, int index);

// <savePath>.display into `d`. A missing or unreadable file leaves `d` at the
// defaults with NotFirstTime 0 -- the PC's state with no registry key -- and
// the first-run pick follows, then the clamp. True when the file was read.
bool display_load(DisplaySettings* d, const char* savePath);

// `d` to <savePath>.display, through a .tmp file and a rename.
bool display_save(const DisplaySettings* d, const char* savePath);

// One line for the debug log: the level and every non-zero value.
void display_describe(const DisplaySettings* d, char* out, int outSize);

// The value index (for display_value) of a name display_describe() prints,
// "envUpd" or "roadRefl"; -1 for none.
int display_index_named(const char* name);
