// The retail collision world (TASKS.md #236).
//
// NFSU2 does not collide against the geometry it draws. It ships a second,
// much coarser dataset authored alongside the art -- retail calls it the
// TopologyTree -- and every question the physics asks about the world is
// asked of that: what is the ground under this wheel, what is it made of,
// what is solid in front of the car, where may the camera sit. The drawn
// mesh is scenery.
//
// The port used to answer those questions from the drawn mesh, because that
// was what it had: model3d scanned a section's triangles and called anything
// steeper than WALK_NZ (about 69.5 degrees from vertical) a wall and anything
// flatter a road. That produces phantom walls out of kerbs, ramps, road
// camber and modelled shopfront detail -- the "collisions in places where I
// shouldn't" the player kept hitting -- and it cannot be tuned out, because
// the drawn mesh simply does not carry the distinction. Retail's data does:
// 39,487 of L4RA's 153,625 collision polygons are flagged solid, and nothing
// else in the city stops the car.
//
// Secondly, it is all resident. The whole of L4RA is 6.7 MB, which the linear
// heap holds without noticing, so the collision answer no longer depends on
// which sections the renderer has paged in, and a section commit no longer
// has to build a physics grid (15.4 ms of a 57.9 ms commit on hardware).
//
// Thirdly (TASKS.md #238), the world is not one tree but one per drivable
// section, and a query reads only the tree of the section it is in -- the
// section a TopologyCoordinate tracks against retail's drivable outlines.
// That matters where two roads cross over each other: each section's tree is
// authored for its own road, so on the lower road of an overpass the lower
// road's tree has the deck flagged as stacked, while the deck's own tree has
// it unstacked, "the only surface here". A single merged grid let the second
// answer win anywhere and lifted the car 14 m onto a race-only deck. The
// coordinates are TOPO_COORD_*: one for the car, one for the camera, as
// retail gives each its own.
//
// tools/topo_pack.py extracts it and documents the retail format in full.
#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#include "model3d.h"   // Model3DWall, MODEL3D_MAX_PROBES
#include "trackpoly.h"

typedef struct Topology Topology;

// The TopologyCoordinates a query goes through. Each remembers its section,
// its tree and the last few polygons it stood on, so the car and the camera
// must not share one: the camera's eye is often in another section. Nor may
// the AI cars share the player's (TASKS.md #280): every retail Car owns its
// own, and one coordinate dragged between six cars a kilometre apart re-picks
// its section on every query and answers a stacked junction from whichever
// car asked last. TOPO_COORD_AI0 + i is AI car i's. TOPO_COORD_LINE walks the
// race route once at a race start, building the AI line (ailine_build).
// TOPO_COORD_PROP is the loose smokeable props' (smokeable.c, #318), which
// retail gives one each; they are few, and near the cars that hit them.
// TOPO_COORD_FLARE finds the road under the world's lamps for their
// reflections (#323), a fresh coordinate each in retail, once a lamp.
#define TOPO_COORD_AI_MAX 5
enum { TOPO_COORD_CAR, TOPO_COORD_CAMERA, TOPO_COORD_LINE, TOPO_COORD_AI0,
	TOPO_COORD_PROP = TOPO_COORD_AI0 + TOPO_COORD_AI_MAX, TOPO_COORD_FLARE,
	TOPO_COORD_COUNT };

// Where topology's own trace goes; shares world.c's sink, set alongside it.
void topo_set_log(void (*sink)(const char* msg));

// `dirPath` is the directory holding WORLD.NFST, the same one world_open()
// was given. Returns NULL when the file is missing or malformed, which is
// not fatal: the caller falls back to the drawn geometry as before, which is
// also what a pack made before #236 gets.
Topology* topo_open(const char* dirPath);
void      topo_close(Topology* t);

size_t topo_bytes(const Topology* t);

// Which of retail's topology groups are solid (TASKS.md #232). 12,049 of
// L4RA's polygons belong to one, and every group is a race's barriers --
// BARRIERS_<track>, PLAYER_BARRIERS_<track>, BARRIERS_CAREER<stage> -- laid
// across roads that are open the rest of the time. Retail switches them all
// off at load (TopologyManager::Init, Speed.elf 0x801f33fc) and back on only
// for the race being run (TrackLoader::InitTopologyAndSceneryGroups,
// 0x80196cb8; Car::SetupTopologyGroups, 0x80221858). Both queries above skip
// a polygon whose group is off.
//
// `trackId` is the track being raced, TrackInfo +0x8A -- 4000 in free roam,
// which names no group. `careerStage` is PlayerCareerState +0x71E4 when in
// career free roam and -1 in a race, where retail leaves the career's
// barriers off. Until this is called only group 0 is solid. Returns how many
// groups it switched on; a version 1 WORLD.NFST has no table, every group
// stays solid, and this returns 0.
//
// It also clears every coordinate, as retail's new cars start with new ones.
int topo_set_groups(Topology* t, int trackId, int careerStage);

// The drivable section `coord`'s tree belongs to (1508, O08, for the tree
// O28), 0 when it is in none, -1 when the file has no section trees. For the
// debug log.
int topo_coord_section(const Topology* t, int coord);
// The tree's own section, the drivable one plus the LOD offset (1528, O28):
// the streamed section that carries the tree, and its event pack
// (emevents.h). 0 in none, -1 without section trees.
int topo_coord_tree_section(const Topology* t, int coord);

// The ground under each of `n` XY points, as TopologyCoordinate::GetElevation
// computes it (Speed.elf 0x801f1484), through coordinate `coord`:
//
//   - the coordinate moves to the point, and on to another section's tree if
//     the point has left its own; its last four polygons are tried first;
//   - only polygons retail marks as road are considered -- `flags & 0x0a`
//     rejects walls and the non-road surfaces (roofs, verges, interiors)
//     before anything else is done with them;
//   - a polygon strictly containing the point and not part of a stack wins
//     immediately, whatever its height, which is what keeps a car on a plain
//     street from ever finding anything else;
//   - otherwise the highest candidate at or below `zMax` wins, and the search
//     stops once one lands within 4 m of it;
//   - if that finds nothing, the whole search runs again with no ceiling, so
//     a car that has ended up under the road still gets an answer rather than
//     falling forever.
//
// outZ/outHit/outN follow model3d_ground_z_multi()'s convention so the two are
// interchangeable: outHit[i] is set only for points that found ground and left
// alone otherwise. `outN`, when not NULL, is 3n long and receives the unit
// normal of the winning polygon. `outTerrain`, when not NULL, is n long and
// receives retail's terrain type for it (the byte TerrainTypeInfoTable is
// indexed by; 1 is road). Returns how many points found ground.
//
// A point outside every drivable section has no tree, and retail no ground
// there; the port reads the merged grid instead, as it does for a file older
// than version 3.
int topo_ground_probe(Topology* t, int coord, const float* xy, int n, float zMax,
	float* outZ, bool* outHit, float* outN, signed char* outTerrain);

// The solid walls within `radius` of (`x`,`y`) whose height band overlaps
// [`zLo`,`zHi`], reduced to the XY segment and band world.c's collision
// solver already works in, appended to `out` which holds `count` of them.
//
// Same keep-the-nearest policy as model3d_walls_near(): a full buffer drops
// its farthest entry for any closer candidate, so what survives truncation is
// what the car is actually near. Unlike that function this is asked once for
// the whole world rather than once per resident section.
//
// The walls come from the tree of the section (`x`,`y`) is in, as
// BuildTrackPolygons (0x801f2d40) takes them, through coordinate `coord`.
int topo_walls_near(Topology* t, int coord, float x, float y, float radius,
	float zLo, float zHi, Model3DWall* out, int count, int max);

// What the cars collide with (TASKS.md #223): TopologyTree::BuildTrackPolygons
// (0x801f2d40) over `box` (min x, min y, max x, max y), through coordinate
// `coord` placed at the box's middle as SetInterestBBox (0x801f1198) places it.
// Every polygon of that tree whose own box meets the query's and whose group
// is on -- road, wall and the 0x08 surfaces alike; the car's body meets all of
// them -- at most `max` (retail's 160, TOPO_TRACK_POLYS_MAX), in retail's
// order: the roads, then the walls, then the 0x08 surfaces. `ids` gets each
// one's index for topo_track_poly(), `boxes` (4 floats each, may be NULL) its
// box. Returns how many. Off every drivable outline the merged grid answers,
// as for the walls above.
#define TOPO_TRACK_POLYS_MAX 160
int topo_track_polys(Topology* t, int coord, const float box[4], u32* ids, float* boxes,
	int max);

// TrackPolygon::TrackPolygon (0x801f0150) on polygon `id`. False for an index
// out of range or a polygon with no area, which has no normal to collide
// along.
bool topo_track_poly(const Topology* t, u32 id, TrackPoly* out);
