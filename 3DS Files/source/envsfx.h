// The world's ambient sounds (TASKS.md #509): the jets over the city, the
// elevated trains and the fountains -- retail's CSTATEMGR_Planes, _Trains and
// _Enviro with their SFXObj_Plane / _Train / _Fountain (Speed.elf
// 0x800AAD5C.., tools/re/wip509/NOTES.md has the whole spec).
//
// Each manager keeps a table of things that can sound (a plane or a train is
// a world animation's entity, worldanim_position; a fountain a fixed point)
// and a few states, the mix map's copies of its state (mixmap.h,
// MIXMAP_PLANE_COPIES etc.). Every frame it attaches a free state to each
// entry the car is near (planes 2000 m, trains 400 m, fountains 150 m) --
// or steals the farthest-attached one -- and detaches the ones it has left or
// whose animation is waiting. An attached state's sound is an ENV_STATIC
// instance out of ENV_COMMON_MB (carsfx.c's s_envBank): sample 0 a train,
// 1 a plane, 4 a fountain; a URL race's planes play FX_MAIN_MEM out of
// IG_GLOBAL/FX_MAIN_MEM_MB, which this loads for them.
#pragma once

#include <stdbool.h>

#include "worldanim.h"

// TheRaceParameters as EnterWorld reads it: the track (free roam 4000), the
// drag flag (+0x40), the URL flag (+0x50), and whether it is a LAN Outrun
// (FEDatabase gameMode 4, race type 8), which keeps a URL track's normal
// planes.
typedef struct {
	int  track;
	bool drag, url, lanOutrun;
} EnvSfxWorld;

// SFX_Common::SetupLoadData's FX_MAIN_MEM_MB, loaded when `url` (the URL mix
// map) on `bus`. carsfxSetup calls it, after the mix map is made.
void envsfxSetup(const char* audioDir, int bus, bool url);
// Every sound let go, every state detached, FX_MAIN_MEM_MB unloaded; the next
// envsfxPreMix enters the world again. carsfxShutdown calls it.
void envsfxShutdown(void);

// Before mixmapUpdate: EnterWorld when `w` or `anims` changed, the three
// managers' UpdateParams, and each attached state's (where its controller's
// object is this frame). `carPos` is the player's car, `dt` the frame's.
void envsfxPreMix(const EnvSfxWorld* w, const WorldAnims* anims, const float carPos[3],
	float dt);
// After it: each attached state's ProcessUpdate (its level, pitch and
// azimuth off the map's outputs).
void envsfxUpdate(void);
