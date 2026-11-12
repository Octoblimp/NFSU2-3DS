#pragma once
#include <3ds.h>
#include <stdbool.h>

// The cars' collision hulls (TASKS.md #316), out of GLOBAL/CARHULLS.nfsg.
//
// Retail's CarCollisionBody (Speed.elf 0x801AFAD4) is a GJK polytope from the
// car's GEOMETRY.BIN, found by name hash -- the default "<CAR>_CV" unless a
// fitted body kit's part names its own -- with the CarPhysicsInfo centre of
// gravity taken off it (MoveCenterOfGravity). tools/hull_pack.py packs every
// car's default hull; the vertices are model space as authored, +X front, +Y
// left, +Z up. The traffic records all carry one generic body box (4.2 x 1.625
// x 1.33 m about a CG 0.25 m up), so the hull is the only place a sedan, a van
// and a bus differ in shape.
//
// The kits' own hulls are not packed: nothing on the 3DS reads a kit part's
// "CV" attribute yet, so a car with a body kit has its stock hull.

#define CARHULL_MAX_VERTS 64

// Reads CARHULLS.nfsg from the folder `bunPath` (GLOBALB.BUN) is in, the
// first time; later calls do nothing. Without the file every car keeps its
// record's body box.
void carhull_load(const char* bunPath);

// How many hulls the file held, for the load log.
int carhull_count(void);

// The default hull of the car CarTypeInfo calls `name`: its vertex count, 0
// with none, and `*verts` its positions, three floats each, resident for the
// session.
int carhull_find(const char* name, const float** verts);
