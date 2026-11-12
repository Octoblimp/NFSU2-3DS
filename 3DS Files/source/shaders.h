// The shared in-world shader blob (TASKS.md #352).
//
// citro3d uploads a vertex program's whole code blob whenever the next
// program bound comes from another DVLB (C3D_BindProgram compares the DVLPs),
// and the upload, with the pipeline drain around it, came at most of the
// ~110 program switches in an in-world frame. Every program drawn in the
// world now comes from one blob, worldsh (the Makefile's SHADER_GROUP), parsed
// once here and never freed, so a switch between them only reconfigures the
// entry point, the output map and the program's constants.
//
// The order is the Makefile's SHADER_GROUP order: keep the two the same.
#pragma once
#include <3ds.h>

typedef enum {
	SHADER_SCENE,      // scene.v.pica: the world's sections, the skydome
	SHADER_SCENEHL,    // scenehl.v.pica: the projected headlight
	SHADER_SCENERF,    // scenerf.v.pica: the road's reflection
	SHADER_CARSH,      // carsh.v.pica: the in-world car's harmonics light
	SHADER_CARENV,     // carenv.v.pica: the in-world car's reflection
	SHADER_CAR,        // car.v.pica: the frontend car
	SHADER_ENVMAP,     // envmap.v.pica: the env sphere's fold
	SHADER_PARTICLE,   // particle.v.pica: flares, shadows, neon, fog, effects
	SHADER_POST,       // post.v.pica: full-screen passes
	SHADER_CARGE,      // carge.v.pica: the in-world car's gloss + reflection (#431)
	SHADER_COUNT
} ShaderId;

// The program's DVLE in the shared blob, or NULL if the blob would not parse.
// The blob is never freed: callers that used to DVLB_Free their own parse just
// drop the pointer.
DVLE_s* shaders_get(ShaderId id);
