#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "race.h"
#include "trackroute.h"

// InitTrackRoutes' race branch (Speed.elf 0x801FFA18, TASKS.md #549) over a
// race's own Routes<track><F|B>.bin.
//
// Retail keeps its racers on the race at load, not as they drive: with the
// race's TrackPath loaded and its barriers closed it rewrites the file's sign
// types per ePostednessType (0 racers, 1 traffic, 2 players) and re-derives
// the posted bits from them:
//
//   RestoreSignposts 0x801FEF44         types back to the file's originals
//   DetermineRelevanceOfRoutesToTrackPath 0x801FE998
//                                       route bool 6: a sign onto it lands
//                                       CloseToPath of the race; bool 9 for
//                                       traffic's, 7 where it runs off an end
//   EliminateHTurns 0x801F6180          corner signs straight back off
//   SetDistanceAroundTrackAndFixupDanglers 0x801F6BDC
//                                       signs to an irrelevant route go
//   CheckForBarrierOverlap 0x801FE828
//   EliminateHTurns / EliminateUTurns 0x801F5CA8 /
//   EliminateBarrierViolations 0x801F5F18, until nothing changes
//   BypassInterpolatedCorners 0x801F6B00
//   ResetPostedness 0x801FDE1C, DeterminePostedness 0x801FE304
//   MakeUnpostedRoutesIrrelevant 0x801FE6EC, FindIrrelevantSignposts 0x801F708C
//
// A racer then needs only the types: IsAlways (2) is taken, Ignore (7, 8)
// never, and the rest at DecideSignpostAsRacer's odds. The decodes are in
// tools/re/wip549fp/racepass*_ps.c. gps.c runs the free-roam branch of the
// same function on RoutesFreeRoam.bin (#358).
//
// PORT: the results go to TRSign fpType / fpGone and TRPoint fpPostF /
// fpPostB, which only FuturePoints reads; the file's own values are put back
// for the port's AiLine, forks and race distances, which were tuned on them
// (rewritten, they doubled the sim's wall time off FuturePoints). The racers
// keep the port's hops (fp_set_race) as well: without them the sim's full
// field hit the walls half as much again. Routes naming a TrackPath of their
// own (+0x58) are measured against the race's path like the rest, and
// SortByDistanceAroundTrack's order of the signs is not kept, only which of
// them it drops past the end.

typedef struct {
	int changes;      // sign types rewritten by the loop's three passes
	int passes;       // times round that loop
	int relevant;     // routes left relevant (bool 6)
	int gone;         // signs dropped (the danglers and FindIrrelevantSignposts)
	int racerSigns[16]; // racer sign types after, by type
	// Against the file as shipped, which holds the posted bits retail's own
	// tools wrote: points whose racer / player posting the pass changed.
	int diffRacer, diffPlayer;
} RacePassStats;

// Runs the pass on `s` in place for track `trackId`. False when either is
// missing (nothing changed).
bool racepass_run(TrackRouteSet* s, const RaceRoute* rr, int trackId, RacePassStats* st);
