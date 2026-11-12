// The props a car knocks over (TASKS.md #318): the highway's crash barrels,
// the street signs, bins, bumpers and water barrels -- retail's Smokeables.
//
// Retail draws each as an ordinary scenery instance with a VirginSmokeable on
// it, a static collision box (Speed.elf 0x801D2BF0). The first car to touch
// the box converts it (ConvertToSmokeable, 0x801D2ED8) into a Smokeable, a
// RigidBody of the SmokeableInfo's mass and inertia that the car's contacts
// push (carsim_prop_contact), and the scenery instance is hidden for the rest
// of the session (SmokeableSpawnerDisabledBits, cleared only when the track
// loads). The Smokeable falls, tumbles and settles (Smokeable::Move
// 0x801D3EC0, ComputeAuxiliaries 0x801D5054), goes to sleep, and is cleared
// away once it is far from the player and the pool runs short
// (DeleteFarAwaySmokeableCollisionBodies 0x801D21F8). One converted by a
// touch that did not wake it turns back into a virgin after 2 s
// (CheckTimers 0x801D45A8).
//
// tools/smokeable_pack.py writes TRACKS/L4RA/WORLD.NFSK from the track's
// SmokeableInfo table, collision volumes and the sections' spawner packs;
// without the file nothing is converted and the cars drive through the props
// as before.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#include "afx.h"
#include "carsim.h"
#include "world.h"

typedef struct Smokeables Smokeables;

// NULL when the file is missing or malformed, which `log` is told; not fatal.
// A fresh one per world load: every spawner enabled again, as SpawnSmokeables
// clears the disabled bits.
Smokeables* smokeable_open(const char* path, void (*log)(const char* msg));
void        smokeable_close(Smokeables* s);

// The particle system the props' emitters run in (the water out of a knocked
// barrel), NULL for none: a card packed before #318 has no prop groups in
// ACIDFX.nfsa and the props are knocked about dry. It must outlive `s`.
void        smokeable_set_afx(Smokeables* s, Afx* a);

// The frame's step, after the cars have moved and before their contacts: the
// virgins follow their sections in and out, the loose props move, settle and
// time out (Smokeable::Move), and SmokeableThink clears away the far ones.
// `player` is where GetDistanceToClosestPlayer measures from.
void smokeable_update(Smokeables* s, World* w, const float player[3], float dt);

// One car against the props, once its pose for the frame is final: each
// virgin its swept body touches is converted, then every loose prop within
// reach goes through carsim_prop_contact.
void smokeable_car(Smokeables* s, World* w, CarSim* car, float dt);

// The world_set_prop_draw() callback, `ud` the Smokeables: each loose prop's
// scenery instance, moved from where it was baked to where the prop is.
void smokeable_draw(const World* w, const C3D_Mtx* vp, float ambient, float gain,
	float invGamma, void* ud);

// For the log: props loose now, and converted, woken and cleared away so far.
typedef struct {
	int active, converted, woken, reverted, killed;
} SmokeableStats;
void smokeable_stats(const Smokeables* s, SmokeableStats* out);
