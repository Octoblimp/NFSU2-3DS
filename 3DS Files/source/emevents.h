// The event manager's triggers (TASKS.md #298): spheres in the world a car
// sets off as it drives into them.
//
// Every streamed section of the city carries an event pack (STREAML4RA.BUN
// chunk 0x80036000): its triggers and an AABB tree over them.
// LoaderEventManager (Speed.elf 0x8020D284) hangs a pack on its section's
// VisibleSectionManager user info while the section is loaded. Each step,
// Car::TriggerEvents (0x80221A10) asks emTriggerEventsInSection (0x8020DB34)
// which triggers of the section its topology is in hold a point 0.15 s ahead
// of it. L4RA's are all sound spheres (EVENT_ID 0x10007): 12 m round a lamp
// post, a pillar or a tree, which become the roadside wooshes (carsfx.c).
// tools/emevents_pack.py copies the 251 packs to TRACKS/L4RA/WORLD.NFSE;
// without the file nothing is set off.
#pragma once

#include <3ds.h>
#include <stdbool.h>

// A trigger, the fields TriggerEvents and the test read (24 bytes, as
// WORLD.NFSE holds them; the 0x20-byte original is in emevents_pack.py).
typedef struct EmEvent {
	u32   eventId;    // EVENT_ID, +0x04
	u32   nameHash;   // +0x08, bStringHash of what it is round
	float pos[3];     // +0x10
	float radius;     // +0x1C
} EmEvent;

#define EM_EVENT_SOUND_SPHERE 0x10007u

// emTriggerEventsInSection collects at most 40 a call.
#define EMEVENTS_MAX 40

typedef struct EmEvents EmEvents;

// NULL when the file is missing or malformed, which `log` is told; not fatal.
EmEvents* emevents_open(const char* path, void (*log)(const char* msg));
void      emevents_close(EmEvents* db);

// emTriggerEventsInSection: the triggers of section `section`'s pack whose
// sphere holds `p` (the squared distance under the squared radius), at most
// `max`, into `out`; the count. Nothing when that section's pack is not
// loaded (`loaded`, NULL = always).
int emevents_trigger(const EmEvents* db, const float p[3], int section,
	bool (*loaded)(int section, void* ud), void* ud, const EmEvent** out, int max);
