#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "rideinfo.h"

// The career's visual rating, "the stars" (TASKS.md #268 B): what a ride's
// visual parts are worth in reputation, and the 0..10 star count the magazine
// shoots pay by (PlayerCareerState::NotifyShowcaseCompleted) and the DVD covers
// unlock by (RecalcDVDUnlocks). Decompiled from SpeedPS.elf, the GameCube
// build with symbols (tools/re/wip268/rep_ps.c, rep2_ps.c, initunl_ps.c,
// stars_ps.c).
//
//   GetNumberOfStars (StarGazerGuide, 0x80184280)
//     the sum of GetCarPartRep over the ride's slots 0..0xA9, skipping the
//     vinyl colours 0x4E..0x5C and the rear wheel 0x1E, and the two bumpers
//     and the skirt (1, 2, 0xB) when the WIDE_BODY part (6) has a level; that
//     rep read off StarValues ({0, 0}, {1, 500} .. {10, 5000}), linearly
//     between its rows -- rep / 500 -- and 10 from 5000 up.
//   GetCarPartRep (PlayerCareerState, 0x80129548)
//     GetUnlockablePartRep(MapCarPartToUnlockable(slot, part), the part's
//     level), 0 for the car's own stock paint in BASE_PAINT, and -1 read as 0.
//   GetUnlockablePartRep (0x8012E294)
//     level 0 is 0; levels 1..3 the unlock table's level record's first short
//     (UnlockablePartData::GetLevel, perfdb.h PerfUnlockLevel.ratingMin);
//     level 4 a constant per entity.

#define STARGAZER_MAX_STARS 10.0f

// The rep and the stars of `r`'s parts. `pdb` is the unlock table; NULL rates
// every part 0.
int stargazer_rep(const RideInfo* r, const PerfDb* pdb);
float stargazer_stars_for_rep(int rep);
float stargazer_stars(const RideInfo* r, const PerfDb* pdb);

// MapCarPartToUnlockable (0x8012E8BC): the eUnlockablePartEntity part `p` in
// CAR_SLOT_ID `slot` prices and unlocks by.
int stargazer_unlockable(const CarPartDb* db, int slot, CarPart p);
