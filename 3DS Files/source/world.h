// The runtime section pager -- the thing that makes the real NFSU2 world
// loadable on a console with 96MB of RAM (TASKS.md #132).
//
// The retail world is not a model, it is 393 streamed sections; L4RA's stream
// container is 118MB and the engine pages the handful of sections around the
// car in and out as it drives. tools/track_pack.py converts those sections one
// .nfsc apiece (see its docstring) and writes WORLD.NFSX next to them: a flat
// index giving each converted section's world bounding box and on-disc cost.
// This is the other half -- the loader that reads that index, keeps the
// sections near a world position resident, and drops the rest.
//
// Nothing here decodes anything. A section is an ordinary Model3D (a baked-
// lighting one, drawn by scene.v.pica), so paging is open/close of files the
// packer already put in GPU order.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#include "model3d.h"
#include "trackpoly.h"

typedef struct World World;

// `dirPath` is the absolute directory holding WORLD.NFSX and WORLD.NFSZ, the
// archive of the sections it names and their texture bank, e.g.
// "sdmc:/nfsu2data/TRACKS/L4RA". Both are required since 2026-10-26 (#270):
// returns NULL when either is missing or malformed, or the archive was built
// from another index, and world_open_error() then says which in a line fit
// for the screen. Opening the world loads nothing -- the first world_update()
// does.
// Where the pager's own trace goes. world.c cannot reach main.c's static
// dlog(), and a pager whose loads and drops are invisible cannot be shown to
// work -- so main.c hands it the sink at startup. NULL (the default) is silent.
void world_set_log(void (*sink)(const char* msg));

World* world_open(const char* dirPath);
void   world_close(World* w);
const char* world_open_error(void);   // "" after an open that worked

// Reads the retail visible-section tables out of the track's resident file
// (`<GameDir>/TRACKS/L4RA.BUN`, copied as-is from the PC install). From then
// on world_update() finds the drivable section the car is in and world_draw()
// draws only the sections retail lists as visible from it. Without the file
// every resident section in range is drawn, as before. Returns false then.
bool world_load_visibility(World* w, const char* bunPath);

// Whether the section numbered `number` (retail's letter * 100 + n, "A21" ->
// 121) is resident. LoaderLights links a section's light pack into the query
// only while the section is loaded, so this is what the light database asks
// (lightdb.h, #166).
bool world_section_loaded(const World* w, int number);

// FindDrivableSection at `eye` and its visible list: the numbers (as above)
// of the resident sections on it, at most `max` into `out`, in the list's
// order. What eRenderWorldLightFlares walks (#323). 0 without L4RA.BUN's
// tables or before the car has been found in a drivable section.
int world_visible_loaded(const World* w, const float eye[3], u16* out, int max);

// The section of the tree topology coordinate `coord` (TOPO_COORD_CAR, ...)
// is on, numbered as above -- the drivable section plus the LOD offset, M36
// for M16 -- which Car::TriggerEvents hands the event manager (emevents.h):
// the streamed section whose event pack holds the triggers round the car.
// 0 in none, -1 without the topology's section trees.
int world_topo_section(const World* w, int coord);

// Pages sections around `pos` (world metres, the retail +Z-up space) for a car
// moving at `vel` (m/s; NULL is standing still). Starts at most ONE section's
// read per call, on the loader thread.
//
// With the visible lists loaded (world_load_visibility()) the order is
// retail's TrackStreamer (TASKS.md #270 P1): the lists of the drivable section
// the car is in and of the one 1.5 s ahead are wanted, ranked by retail's
// loading priority; the lists of the sections around it are read ahead into
// spare memory; nothing wanted is ever evicted, and what nobody wants goes
// oldest first, only when its memory is needed. Without them, the nearest
// missing section inside a radius. Returns true when the resident set
// changed, which is only useful for logging.
bool world_update(World* w, const float pos[3], const float vel[3]);

// IsWorldDataStreaming (Speed.elf, CARSFX_Rain's thunder gate): a section is
// being read or bound this frame.
bool world_streaming(const World* w);

// The blocking form of world_update(), for filling the world before the first
// frame. world_update() hands its reads to a loader thread and returns
// immediately (#176); this waits for them, which is what the loading screen
// is for. Only what is wanted at `pos` is loaded here -- read-ahead waits for
// the frames. Call it until it returns false: true means it did something --
// loaded (or tried) one more section, finished the one in flight, filled a
// texture slot or dropped a section -- and the next call may find more.
bool world_prime(World* w, const float pos[3]);

// A race start, a return to free roam, a reset (TASKS.md #270 P5): drops every
// resident section the streamer has no use for at `pos`, before
// world_prime() fills in what is wanted there. Far scenery stays.
void world_retarget(World* w, const float pos[3]);

// What world_prime() calls while it waits on a read -- one frame of whatever
// loading screen is up -- so the screen keeps moving through a long read.
// NULL (the default) waits without drawing.
void world_set_prime_tick(void (*tick)(void* ud), void* ud);

// Draws every resident section within the draw radius of `pos` through `vp`.
// `ambient`/`gain`/`invGamma` are scene.v.pica's exposure controls, as passed
// to model3d_draw_scene().
void world_draw(const World* w, const C3D_Mtx* vp, const float pos[3],
	float ambient, float gain, float invGamma);

// One face of the car's env map (TASKS.md #166): the scenery retail's env
// views draw (DrawAScenery's 0x1000 mask, each instance's env model at
// P >= 32), from `eye` along unit `fwd` through the 90-degree face camera
// `vp`. The target and viewport are the caller's. Returns the triangles drawn.
u32 world_draw_env(const World* w, const C3D_Mtx* vp, const float eye[3],
	const float fwd[3], float ambient, float gain, float invGamma);

// The scenery of the road's reflection (#296, reflection.h): eDisplayFrame's
// views 4 and 5, the env set less the terrain, each instance mirrored about
// its own origin, culled by its real box in the player's frustum `vp` with
// world_set_lod_view()'s camera. world_draw()'s sections around `pos`; the
// target and viewport are the caller's. Returns the triangles drawn, and the
// mirror changes they took in `outMirrors` (may be NULL).
u32 world_draw_reflection(const World* w, const C3D_Mtx* vp, const float pos[3],
	float ambient, float gain, float invGamma, u32* outMirrors);

// The smokeable props (smokeable.c, TASKS.md #318). A number for the section's
// current load -- a new one each time its model goes in, 0 while it has none
// -- so a caller holding its instances knows when they went away.
u32 world_section_serial(const World* w, int number);
// The instance of section `number` (any of its parts) whose translation is
// `pos`: its model and index. False when the section is out or has none.
bool world_find_instance(const World* w, int number, const float pos[3],
	Model3D** outModel, u32* outInst);
// The props' light flares (carlights.h, TASKS.md #323): which model the last
// world_draw() drew the instance of section `number` at translation `pos`
// at -- 1 its A, 2 its B, 0 neither or the section is out. `serial`, `model`
// and `inst` are the caller's, zeroed to start, and remember the instance
// across frames; it is looked for again when the section's load changes, at
// most a few a frame (0 meanwhile). -1, once a load, when it is not there.
int world_instance_lod(const World* w, int number, const float pos[3], u32* serial,
	Model3D** model, u32* inst);
// The world animations' models (worldanim.c, TASKS.md #239): track_pack.py
// bakes them into the pinned SECTION_ANIM section(s), WANIM and any WANIM.n
// it was split into, which world_draw() never draws itself. How many parts
// there are, and part `part`'s model with its load's serial (as
// world_section_serial) -- NULL and 0 while it is out.
int world_anim_parts(const World* w);
Model3D* world_anim_part(const World* w, int part, u32* serial);
// Called at the end of world_draw(), after both phases, with the cull planes
// off and the LOD view still set: the props a car knocked loose. NULL: none.
// `w` is the world being drawn, for the callback's section checks.
typedef void (*WorldPropDraw)(const World* w, const C3D_Mtx* vp, float ambient,
	float gain, float invGamma, void* ud);
void world_set_prop_draw(WorldPropDraw fn, void* ud);

// The camera the scenery's levels of detail are picked from (TASKS.md #247):
// its position, its unit forward vector, and its field of view in the game's
// 16-bit angle units (g_chase.fov, the GameCube camera's +0xc4). Each instance
// then takes its A model, its B model or nothing by retail's own rule
// (DrawAScenery; see model3d.c). Set it before world_draw() each frame; NULL
// draws every instance at A.
void world_set_lod_view(const World* w, const float eye[3], const float fwd[3],
	float fovU16);

// The level of detail CarRenderInfo::Render draws a car at from that same view
// (TASKS.md #296): 0 (A) .. 3 (D), never below `minLod`, or -1 for a car too
// small on screen to draw at all -- body, shadow, neon and flares alike.
// `minLod` with no view set or DbgNearLod on.
int world_car_lod(const World* w, const float pos[3], int minLod);
// PORT (TASKS.md #380): traffic's level, one swap size further out than
// retail's -- A while CarBodyLodSwapSize[1] (45) is met, about 30 m, rather than
// [0] (120), about 13 m, so the cars beside the player's are the detailed ones
// (the user's "as detailed as my car if possible", #316). Same -1 and minLod.
int world_traffic_lod(const World* w, const float pos[3], int minLod);
// GetPixelSize's answer itself, 0 with no view set.
int world_car_pixels(const World* w, const float pos[3]);
// How far a car at C is from B's swap size down to C's (CarBodyLodSwapSize[1]
// and [2]): 1 at B's, 0 at C's. CarRenderInfo::Render fades the hull shadow
// of a car at C by it (carshadow_draw_hull).
float world_car_lod_c_fade(const World* w, const float pos[3]);
// The same for world_traffic_lod()'s C (its own B and C swap sizes).
float world_traffic_lod_c_fade(const World* w, const float pos[3]);

// Where to put a car when there is nowhere else to put it: the centre of the
// index's smallest-area section, which is the closest thing to "a street" the
// index can offer -- the big-area sections are the ones carrying skyline and
// distant-LOD shells (see world.c). Returns false on an empty index.
//
// This is a placeholder for the real start line, which lives in track data
// this project has not decoded yet.
bool world_spawn(const World* w, float out[3], float* outYaw);

// Diagnostic: replaces BANK_LOD_BIAS for bank textures acquired from now on
// (TASKS.md #191). A texture already resident keeps the bias it was made with.
void world_set_lod_bias(float bias);
// Diagnostic (DbgTexBase, TASKS.md #328): the mip level the next world_open()
// uploads its textures from, 0..3, instead of the one it measures room for;
// -1 measures.
void world_force_tex_base(int level);
void world_set_split_load(bool on);
// Diagnostic: skip the pinned far-scenery sections, draw every scenery
// instance at its A model whatever its size on screen (#247), and/or turn the
// sub-section frustum cull (TASKS.md #68) off so a run with it and a run
// without it can be compared without rebuilding.
void world_set_draw_debug(bool noFarScenery, bool nearLod, bool noCull);
// world_draw() starting the GPU on the frame while it is still recording the
// rest (TASKS.md #68; world.c's "GPU kicks"). On by default; off is DbgNoKick
// and the GPU sweep's "nokick" step.
void world_set_gpu_kicks(bool on);
// The heap flush at a kick (#352): false cleans the cache straight through the
// kernel (the default), true flushes it through GSP as before (DbgGspFlush).
void world_set_gsp_flush(bool on);
// One kick outside world_draw(), when kicks are on: main.c's at the world
// frame's stage boundaries for the first frames after entering (TASKS.md
// #307), so a stalled list in gpuwatch.c's report names the stage it holds.
// Counts towards world_gpu_frame_end()'s like world_draw()'s own.
void world_gpu_kick(void);
// #349: a kick with no heap flush after the frame's first, for lists that draw
// only what the last flush covered (the env map's faces and fold: static
// sections, the env quad and the sphere's vertices). Stops at KICK_MAX.
void world_gpu_kick_clean(void);
// #521 round 3: world_gpu_kick() for the lists ahead of world_draw() -- the
// road's reflection -- whose vertices the CPU may have just written, so it
// flushes the heap every time; stops at KICK_MAX as the clean one does, which
// leaves world_draw()'s own first kick its room.
void world_gpu_kick_early(void);
// #352: the bank's textures in VRAM -- 0 none, 1 the ground's, 2 the ground's
// and then the rest while there is room (the default). Takes effect at the
// next world_vram_tick().
void world_set_vram_tex(int mode);
// Call between frames, before C3D_FrameBegin(): the moves are DMA copies
// that wait for the GPU's queue and then for themselves, which only works
// outside a frame (on hardware the CPU may not write VRAM at all). Sorts the bank after a mode change (all of it)
// and every 16 frames moves up to 256 KB more into VRAM as room appears.
void world_vram_tick(World* w);
// Call right before the C3D_FrameEnd() of a frame that may have drawn the
// world. Returns how many kicks this frame's world_draw() issued and clears
// the count. When that is not 0 the GPU already had part of the frame: this
// has flushed the linear heap, waited for that part to finish and stopped the
// queue, so pass GX_CMDLIST_FLUSH and C3D_FrameEnd() skips its own flush.
// *outFirstTick is then the first kick's svcGetSystemTick(), where the GPU's
// time on the frame starts.
int world_gpu_frame_end(u64* outFirstTick);
// #345: the ticks the whole-heap cache flushes of the kicks took, and how
// many there were, since the last call (which clears them).
void world_flush_stats(u64* ticks, u32* count);
// Whether bank textures uploaded from now on take their mip chains. On by
// default since #252 (see world.c s_bankMips); off is the #229-#252 frame.
void world_set_mip_chains(bool on);
// Diagnostic (DbgGpuSweep, TASKS.md #252): let the ground textures -- roads,
// sidewalks, plazas, which ship sampling their base level only -- take the
// chain too. Applied by the next world_set_tex_mode().
void world_set_ground_mips(bool on);
// Diagnostic, the same task: which levels of those chains the GPU may sample,
// applied to every resident texture at once and to each one uploaded after.
// Only meaningful once the chains are on; without them every texture is its
// base level anyway.
typedef enum {
	WORLD_TEX_BASE,      // level 0 only: the #229-#252 frame, from the chain's memory
	WORLD_TEX_CHAIN,     // the whole chain, ground excepted: what ships
	WORLD_TEX_TO_1,      // the chain, no further down than level 1
	WORLD_TEX_TO_2,      // no further down than level 2
	WORLD_TEX_SMALLEST,  // the last level only: texture fetch all but free
} WorldTexMode;
void world_set_tex_mode(World* w, WorldTexMode mode);
// g_TextureFiltering (#326): the bank textures' magnification and
// minification filter, bilinear or point (FUN_005d1e20's D3DTEXF_LINEAR and
// D3DTEXF_POINT), for every resident texture and each one uploaded after.
// The mip filter stays linear.
void world_set_tex_filter(World* w, bool linear);

// Height of the road surface under `pos`, searched among the resident
// sections. `headroom` is how far *above* `pos[2]` a surface may still count,
// which is what lets a car that is a little way into a rise climb it instead
// of driving through it; surfaces higher than that are ignored, so an
// overpass overhead does not pull the car off the street. False when nothing
// drivable is under the point (off the edge of the loaded world, or over a
// hole), and the caller should keep the height it has.
bool world_ground_z(const World* w, const float pos[3], float headroom, float* outZ);

// The same query for up to MODEL3D_MAX_PROBES points at once, which is how the
// car asks about its four wheel contacts. `xy` is n interleaved pairs and
// `zMax` is the absolute ceiling (world_ground_z() takes a relative headroom;
// here the points are at different heights so one shared ceiling is simpler).
// outZ/outHit are n long and are filled in by this call. Returns how many
// points found ground; a point that found none keeps outHit[i] false and the
// caller should hold the height it has for that wheel.
//
// This exists because the per-point version is a full triangle scan, and a
// section can carry twenty thousand of them: asking four times per frame costs
// four scans for one car. `outN` (3n long, may be NULL) gets each hit's unit
// surface normal; a miss reads straight up.
int world_ground_probe(const World* w, const float* xy, int n, float zMax,
	float* outZ, bool* outHit, float* outN);

// What a world_collide() call actually saw, for the debug readout -- a car
// that will not move needs to be able to say whether it is against one wall
// or inside a hundred of them, and how high off the road the thing stopping
// it is. Optional; pass NULL in the normal path.
typedef struct {
	int   gathered;    // segments within reach of the move
	int   blocking;    // pushes applied, counting each relaxation pass
	float firstZLo;    // the first pusher's band, relative to the query's zLo
	float firstZHi;
} WorldCollideInfo;

// Moves the car's oriented footprint from `from` to `to` without letting it
// through the near-vertical geometry of the resident sections (TASKS.md #137,
// #141). `to`'s XY is corrected in place; its Z is left alone, because height
// belongs to the ground probe and the two are not allowed to argue.
//
// `yaw` orients the footprint and `halfLength`/`halfWidth` size it, both from
// the car record's real body box, so the car collides as the shape it is drawn
// as rather than as a circle around it. [`zLo`,`zHi`] is the band of the body
// in world heights -- a face that ends below it is a kerb to be climbed, not a
// wall.
//
// Sliding, not stopping: only the component of the move into a wall is taken
// away, so a car that clips a facade at an angle runs along it. When it
// returns true, `outNormal` (optional) holds the unit XY direction of the
// hardest push, which is the surface normal of the thing actually hit -- the
// physics needs it to take the speed out of a crash instead of teleporting the
// car clear with its momentum intact. `outArm` (optional) holds where on the
// footprint that push touched it, as an XY offset from the footprint's centre:
// the lever arm the wall impulse turns the car with (#150).
bool world_collide(const World* w, const float from[3], float to[3],
	float yaw, float halfLength, float halfWidth, float zLo, float zHi,
	float outNormal[2], float outArm[2], WorldCollideInfo* info);

// world_ground_probe() and world_collide() through another car's topology
// coordinate, TOPO_COORD_AI0 + i for AI car i (TASKS.md #280). The two above
// are these with TOPO_COORD_CAR, the player's. `outTerrain` (n long, may be
// NULL) gets each hit's terrain type, as topo_ground_probe() gives it; the
// mesh fallback has none and leaves ROAD (1).
int world_ground_probe_coord(const World* w, int coord, const float* xy, int n,
	float zMax, float* outZ, bool* outHit, float* outN, signed char* outTerrain);
bool world_collide_coord(const World* w, int coord, const float from[3], float to[3],
	float yaw, float halfLength, float halfWidth, float zLo, float zHi,
	float outNormal[2], float outArm[2], WorldCollideInfo* info);
// The solid walls world_collide_coord() would gather around (x, y), nearest
// first when more than `max` are in reach: the AI line's wall pass
// (ailine_build, TASKS.md #280). Topology only -- 0 for a pack without one.
int world_walls_near_coord(const World* w, int coord, float x, float y, float radius,
	float zLo, float zHi, Model3DWall* out, int max);
// The car's collision polygons (TASKS.md #223): topo_track_polys() and
// topo_track_poly() through coordinate `coord`. -1 and false without
// topology, and then the car keeps the footprint resolver above.
int world_track_polys_coord(const World* w, int coord, const float box[4], u32* ids,
	float* boxes, int max);
bool world_track_poly(const World* w, u32 id, TrackPoly* out);

// Keeps a chase camera's eye out of the walls (#211): retail's
// CameraMover::MinGapTopology wall half, over the same wall segments the car
// collides with. `target` is the car's middle; `eye` is moved in place and the
// return says whether it was.
bool world_camera_gap(const World* w, const float target[3], float eye[3]);

// Which race's barriers stop the car and carry it (#232), and which barriers,
// start/finish lines and shop beams are drawn (#237): topo_set_groups() on
// the collision set and scenery_set_groups() on the drawn one, so see
// topology.h and scenery.h. Called on entering free roam with track 4000 and
// the career stage, and per race track with stage -1. `hasCareerCar` matters
// on career stage 0 only; `reverse` is the race running backwards.
void world_set_groups(World* w, int trackId, int careerStage,
                      bool hasCareerCar, bool reverse);

int  world_section_count(const World* w);
int  world_resident_count(const World* w);
// How many sections the last world_draw() submitted after distance and frustum
// culling. The resident count alone says nothing about what a frame costs.
int  world_drawn_count(const World* w);
// How many scenery instances the last world_draw() drew at their A model and
// at their B (#247), for the debug readout. Zeros for a pack from before #247,
// which has no instances to count.
void world_drawn_lod(const World* w, u32* outA, u32* outB);
// What those sections actually cost: draw calls and triangles submitted by the
// last world_draw(), after the per-batch and per-cell tests inside each one
// (TASKS.md #68). Once the cull can cut a section down to part of itself, the
// section count stops being a measure of anything, and these two are what
// replaces it. The car, the shadow and the skydome are not in the number --
// they are drawn outside world_draw() with the cull off.
void world_drawn_submitted(const World* w, u32* outCalls, u32* outTris);
u32  world_resident_bytes(const World* w);
u32  world_budget_bytes(const World* w);

// Shared-texture-bank pixels currently uploaded, and how many mip levels the
// upload is skipping off the top of every chain (0 = the packed resolution).
// Both are in the debug readout because the base level is DERIVED from the
// linear heap the kernel granted, so the only way to know which one a given
// console picked is to have it say so.
u32  world_bank_bytes(const World* w);
u32  world_tex_base_level(const World* w);

// Name of the resident section containing `pos`, or NULL. For the debug
// overlay -- the retail section names are the authored ones ("C22"), so this
// is the one string that says where in the city you actually are.
const char* world_section_at(const World* w, const float pos[3]);
