// The world animations (TASKS.md #239): retail's CWorldAnim* system, which
// moves the jets over the city, the elevated graffiti trains, the tower
// cranes, the sweeping spotlights and the pub's sails.
//
// Retail: a WorldAnimInstance places a tree of entities, each a SpaceNode
// whose CWorldAnimCtrl plays an EAGL anim into the node's local matrix; an
// entity with a model draws it through a WorldModel at the node's world
// matrix. World::DoTimestep runs CAnimPlayer::UpdateTime -> each entity's
// UpdateTimeStep (Speed.elf 0x80048b00): AdvanceAnimTime (0x800557e4) always,
// and the pose (UpdateAnimPose 0x80055b48, BuildSQT 0x802a7078) only for an
// instance with flag 0x2000 or, with no trigger zone, whose box is in view.
// RenderWorldModels (0x8024d3d0) draws each model with no LOD, culled by its
// pixel size at radius 35 and its box in the view. tools/re/wip239/NOTES.md
// has the whole spec.
//
// tools/track_pack.py (world_anim.py) writes TRACKS/L4RA/WORLD.NFSW -- the
// trees, the instances and a pose per frame -- and bakes the models into the
// pinned section WANIM, which world_draw() never draws itself; this draws
// each model out of it through its entity's matrix (model3d_draw_instance).
//
// L4RB's FwdRev tree counts as moving: its trigger is the load itself. The
// shop doors' trees (a trigger zone, NFSW v3) hold frame 0 until a shop NIS's
// DOOR_POSITION event names their zone (worldanim_door, #408); a v2 file
// (L4RD) has no doors -- their scenery stand-ins draw them there.
#pragma once

#include <3ds.h>
#include <citro3d.h>

#include "world.h"

typedef struct WorldAnims WorldAnims;

// NULL when the file is missing or malformed, which `log` is told; not fatal.
// A fresh one per world load, every ctrl at time 0 as InstantiateAnimTree
// leaves it. `track` and `reversed` are TheRaceParameters' +0 and +4 as the
// load reads them (free roam: 4000, forward): an instance's race gate picks
// the tracks it is made for, and a FwdRev tree runs backwards on a reversed
// race.
WorldAnims* worldanim_open(const char* path, int track, bool reversed,
	void (*log)(const char* msg));
void        worldanim_close(WorldAnims* a);

// World::DoTimestep's share: every ctrl's time, then the poses the gate lets
// through. The view gate reads the last frame's view-projection.
void worldanim_update(WorldAnims* a, const World* w, float dt);

// RenderWorldModels' share of world_draw(): every entity's model, culled as
// retail culls it. `vp` is also kept for the next update's view gate.
void worldanim_draw(WorldAnims* a, const World* w, const C3D_Mtx* vp, float ambient,
	float gain, float invGamma);

// A shop NIS's DOOR_POSITION (BroadcastEventToWorldAnims, Speed.elf
// 0x8004a104, matching the instances whose trigger zone is the shop's ->
// SetAnimToNormalizedPosOverTime 0x80056da8): `time` > 0 frames plays each
// door's next leg over that long; 0 snaps it to `pos`, 1 open or 0 shut.
// Returns the instances it moved.
int  worldanim_door(WorldAnims* a, u32 zone, float pos, float time);
// GetAnimPosition (Speed.elf 0x80055E80, #509): where the entity named
// `nameHash` (the root's or a child's node name) of the instance made with
// `id` (WorldAnimInstance +0) is -- its node's world translation -- and
// whether its ctrl is in a start or loop delay. False when no such instance
// was made (another set, the race gate) or it has no such entity, or the
// file predates v4.
bool worldanim_position(const WorldAnims* a, u32 id, u32 nameHash, float pos[3],
	bool* delaying);
// While on, every door is posed each update (retail: the NIS is playing or
// the 10 s door timeout after it runs); off, a door keeps its last pose.
void worldanim_force_pose(WorldAnims* a, bool on);

// For the log: instances, entities with a model, the models found in WANIM,
// and the entities the last draw drew and the last update posed.
typedef struct {
	int instances, drawable, models, found, drawn, posed;
} WorldAnimStats;
void worldanim_stats(const WorldAnims* a, WorldAnimStats* out);
