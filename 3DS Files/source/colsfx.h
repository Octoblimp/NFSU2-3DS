// The collision sounds: retail's EAXCollision (Speed.elf 0x800800B8 on), its
// stSoundCollision objects (stWorldCollision, stCarOnCarCollision) and the
// stitch player they play impacts through (cSTICH_PlayBack, cStichWrapper,
// cSampleWarpper), fed by the physics' Collision objects (Collision::
// AddImpulse 0x801AB990, Update 0x801AC5EC). TASKS.md #298.
//
// An impulse between two bodies opens a Collision, which lives 0.3 s past
// its last impulse. Within 100 m of the player, its first impulse sizes an
// impact (the summed impulse through a Slope, by the surface it reads off
// the hit) and queues it; at most eight play at once, the player's and the
// nearest first. An impact is a stitch: two to five AEMS samples from
// MB_Stich_Collision (AEMS_StichCollision), picked by the surface and the
// impact's strength from GLOBAL/InGameCommon.bun's sound stitches. A world
// collision sliding along also plays FX_Scrape (FX_Misc_MB), at the slip's
// level, faded out over 0.1 s when the collision ends.
//
// Each playing collision holds one of the mix map's five Collision state
// copies (CSTATEMGR_Collision), whose SFXObj_Collision gives the impact and
// the scrape their volume and azimuth (mixmap.h).
//
//   EAXCollision       ctor 0x800800B8, Update 0x800807E4,
//                      CreatCollisionSample 0x800809C0,
//                      StartCollisionSample 0x80080B1C,
//                      ReleaseCollisionSample 0x80080C54, PlayScrape 0x8008090C,
//                      QueueImpactCollision 0x8008035C,
//                      UpdateQueueImpact 0x80080404
//   stSoundCollision   Initialize 0x80081070, Update 0x8008129C,
//                      SetUpCollisionEnding 0x80081000, BeginImpact 0x80081E1C,
//                      EndImpact 0x80081D80, BeginScrape 0x80081B84,
//                      EndScrape 0x80081DD4, UpdateScrapeParams 0x800818F8,
//                      UpdateCollisionEnding 0x800813B4,
//                      GenImpactIntensity 0x80081A30,
//                      GenScrapeIntensity 0x80081B14,
//                      CSISBeginScrape 0x80081494, CSISUpdateScrape 0x80081624
//   stWorldCollision   GetSurfaceType 0x80083D3C, GenImpactIntensity 0x80083F38,
//                      GenStichData 0x80083FEC, ShouldBeginImpact 0x800843DC
//   stCarOnCarCollision GetSurfaceType 0x8008383C, GenStichData 0x80083AFC,
//                      Initialize 0x80083CE8
//   cStichWrapper      Play 0x8008BC80, Update 0x8008BDFC
//   cSampleWarpper     Play 0x8008C344, Update 0x8008C11C, Destroy 0x8008C03C
//   LoaderSoundStichs  0x8008C7B4 (the 0x8003B500 chunks)
//   SFXObj_Collision   UpdateParams 0x800A27F8, ProcessUpdate 0x800A298C,
//                      NewAttachment 0x800A2B44
#pragma once

#include "carsim.h"

#include <stdbool.h>
#include <stdint.h>

// Loads the stitches from `bunPath` (GLOBAL/InGameCommon.bun) and the banks
// from `audioDir` (audio/, its trailing slash): IG_GLOBAL's
// MB_Stich_Collision, MB_Stitch_Whoosh and FX_Misc_MB, on the mix map's bus
// when it is up (carsfxSetup loads it first), else the effects bus. `player`
// is the human player's car. Hooks carsim's impulses. A missing piece leaves
// the collisions silent, nothing else.
void colsfxSetup(const char* audioDir, const char* bunPath, const CarSim* player,
	uint32_t* rng);
// Ages the collisions and runs each Collision copy's SFXObj_Collision
// UpdateParams, before mixmapUpdate. `paused`: the pause menu is up.
void colsfxPreMix(bool paused, float dt);
// ProcessUpdate, then EAXCollision::Update, after mixmapUpdate.
void colsfxUpdate(float dt);
// EAXSound::PlayCameraSnapShot (0x8007D0E8): the photo shoot's camera sound,
// SFX_Common's FX_Camera (FX_Misc_MB), made at the next colsfxPreMix.
void colsfxCameraSnapShot(void);
// Ends every collision sound and unloads (leaving the world). Every stitch
// below stops with it; its handle is then dead, and freeing it does nothing.
void colsfxShutdown(void);

// The stitch player for the other sounds that new their own cStichWrapper
// (road noise's transitions): GetStich(type, idx) played with SND_Params `p`
// (+4 volume, +8 pitch, +0xC azimuth), NULL when there is no such stitch, no
// setup, or the pool (12) is full. Playing is the wrapper's +0x1C, which
// Update clears once every sample is done.
typedef struct ColsfxStitch ColsfxStitch;
ColsfxStitch* colsfxStitchPlay(int type, int idx, const int32_t p[5]);
bool colsfxStitchPlaying(const ColsfxStitch* x);
void colsfxStitchUpdate(ColsfxStitch* x, const int32_t p[5]);
void colsfxStitchFree(ColsfxStitch* x);
