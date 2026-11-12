// Retail's scenery groups: which race barriers, start/finish lines and
// free-roam shop beams are on screen (TASKS.md #237).
//
// Not every scenery instance is drawn all the time. ScenerySectionHeader::
// DrawAScenery (Speed.elf 0x80208fa4) skips one whose flags meet the view's
// mask, and two of those flags are game state: 0x10, which only EnableGroup
// (0x80196bd8) clears, on the members of a named group; and 0x04/0x08, the
// start/finish line drawn for one race direction and hidden for the other.
// TrackLoader::InitTopologyAndSceneryGroups (0x80196cb8) picks the groups
// when a track loads: the race's own BARRIERS_<track> and
// PLAYER_BARRIERS_<track>, and in career free roam FREE_ROAM (the shop beams
// and spotlights) and BARRIERS_CAREER<stage>.
//
// The port bakes those instances into the world sections like any other and
// tags each batch with the "set" its instances share -- the hidden bit, the
// groups that can clear it and the direction bits, in batch flags bits 16-27
// (tools/track_pack.py, SceneryGroups). WORLD.NFSG lists every set, and this
// turns them on and off: a set is drawn when it is not hidden or one of its
// groups is on, and its direction is not the one the race hides.
//
// A world packed before #237 has no tagged batches and no WORLD.NFSG, and is
// drawn exactly as it was. Tagged sections with no WORLD.NFSG draw none of
// the tagged batches: a race barrier across a free-roam street is the worse
// of the two ways to be wrong.
#ifndef SCENERY_H
#define SCENERY_H

#include <3ds.h>
#include <stdbool.h>

// Set ids are 12 bits (track_pack.BATCH_SCENERY_SET_MAX), one bit each here.
#define SCENERY_MAX_SETS   4096
#define SCENERY_SET_BYTES  (SCENERY_MAX_SETS / 8)

typedef struct Scenery Scenery;

void scenery_set_log(void (*sink)(const char* msg));

// WORLD.NFSG from the world directory. NULL when it is missing or unreadable,
// which is said in the log.
Scenery* scenery_open(const char* dirPath);
void     scenery_close(Scenery* s);

// The groups InitTopologyAndSceneryGroups turns on for `trackId`, and the
// sets that leaves drawn, written into `on` (SCENERY_SET_BYTES, bit n = set
// n). `careerStage` is -1 outside career free roam, as for
// topo_set_groups(); `hasCareerCar` picks FREE_ROAM_STAGE0A or _STAGE0 on
// stage 0; `reverse` is the race running backwards. With no `s`, every set
// is off. Returns how many sets are on.
int scenery_set_groups(const Scenery* s, int trackId, int careerStage,
                       bool hasCareerCar, bool reverse, u8* on);

#endif
