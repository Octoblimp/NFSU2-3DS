// Real NFSU2 geometry on the 3DS GPU -- cars and frontend rooms alike.
//
// Reads the .nfsc blob built by tools/car_pack.py (out of the retail
// CARS/<car>/GEOMETRY.BIN + TEXTURES.BIN) or tools/scene_pack.py (out of
// FRONTEND/PLATFORMS/<scene>.BIN), and draws it with citro3d. The blob is
// already in GPU order -- one shared vertex pool, one shared u16 index pool,
// one batch per retail 0x134b02 material subset, PICA200-swizzled 16bpp
// textures -- so loading is a header parse plus a copy into linear memory, and
// drawing is a straight run down the batch list. Nothing is decoded or
// transformed at runtime.
//
// Cars and rooms share this one loader because they share the retail
// 0x80134000 geometry tree; the only real difference is what the vertex's
// second float3 carries. A car's is a normal and gets lit here (its D3DCOLOR
// is a constant 0xFF000000, so there is no baked shading to reuse); a room's
// is the baked vertex lighting itself, unpacked from its D3DCOLOR. The blob's
// `shading` field says which, and picks the vertex shader accordingly.
//
// The .nfsc layout this mirrors is documented at the top of tools/car_pack.py.
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

typedef struct Model3D Model3D;

// Loads a .nfsc from an absolute path (e.g. "sdmc:/nfsu2data/CARS/240SX.nfsc").
// Returns NULL if the file is missing or malformed; callers are expected to
// treat that as "no 3D viewport" rather than as a fatal error.
Model3D* model3d_load(const char* path);

void model3d_free(Model3D* m);

// True when the blob was packed as a baked-lighting room rather than a car.
bool model3d_is_scene(const Model3D* m);

// Builds the combined view-projection for a scene camera. World space is the
// retail one straight out of GEOMETRY.BIN: +Z up, one unit = one metre (cars
// and rooms are authored 1:1, so a car needs no rescaling to stand on a disc).
// `fovDegrees` is vertical; `aspect` is the target's width/height, e.g.
// C3D_AspectRatioTop.
void model3d_camera(C3D_Mtx* out, const float eye[3], const float target[3],
	float fovDegrees, float aspect, float nearZ, float farZ);

// The same with the view's up vector given rather than world +Z, for a camera
// that rolls (retail's chase camera tips its up with the car's acceleration).
void model3d_camera_up(C3D_Mtx* out, const float eye[3], const float target[3],
	const float up[3], float fovDegrees, float aspect, float nearZ, float farZ);

// One eye of the same view for the stereoscopic top screen (stereo.h): the
// eye `iod` / 2 to the side -- negative for the left -- and the frustum
// sheared so a point `screen` units ahead lands where it does in the plain
// view, on the screen's plane.
void model3d_camera_up_stereo(C3D_Mtx* out, const float eye[3], const float target[3],
	const float up[3], float fovDegrees, float aspect, float nearZ, float farZ,
	float iod, float screen);

// Draws a baked-lighting room through `vp`. `ambient`, `gain` and `invGamma`
// are the exposure controls documented in scene.v.pica: the baked colour is
// raised to `invGamma`, scaled and lifted. Pass invGamma = 1 for a plain
// lift-and-gain.
void model3d_draw_scene(const Model3D* m, const C3D_Mtx* vp, float ambient,
	float gain, float invGamma);

// The same for a frontend room whose turntable turns with the car on it: the
// disc's batches (tools/scene_pack.py's BATCH_TURNTABLE, DISC01) turned `yaw`
// radians about the disc's centre, the rest as model3d_draw_scene() draws
// it. A room packed without the tag is drawn still. Its textured batches take
// SPEED2.EXE's FE effect, twice texture times the vertex colour, so pass
// ambient 0, gain 1, invGamma 1 for the PC's exposure.
void model3d_draw_scene_turned(const Model3D* m, const C3D_Mtx* vp, float yaw,
	float ambient, float gain, float invGamma);

// One side of that draw (#319): the room without its turntable, which does
// not change while the camera holds still and so can be drawn once and kept
// (fe_cache.h), or the turntable alone. The parts together draw what
// model3d_draw_scene_turned() does, except that a room with blended batches
// gets the room's blended ones before the disc's opaque ones -- none of the
// shipped rooms has any. A room packed without the tag is all room.
typedef enum {
	MODEL3D_SCENE_ROOM,
	MODEL3D_SCENE_DISC,
} Model3DScenePart;
void model3d_draw_scene_part(const Model3D* m, const C3D_Mtx* vp, float yaw,
	float ambient, float gain, float invGamma, Model3DScenePart part);

// The same draw at a chosen level of detail. A world section's blob carries its
// scenery twice -- the A model of every prop and the B one the retail scenery
// record names beside it (tools/track_pack.py) -- and `farLod` picks which of
// the two the batch loop keeps, for the whole model at once.
//
// A blob packed since TASKS.md #247 carries an instance table as well, and
// then `farLod` is not read: each instance takes its own level every frame by
// retail's rule (model3d_set_lod_view). `farLod` is only for older packs.
//
// Passing a model with no LOD split draws all of it, whichever way this is
// called, so it is always safe.
void model3d_draw_scene_lod(const Model3D* m, const C3D_Mtx* vp, float ambient,
	float gain, float invGamma, bool farLod);

// Which half of the two-pass batch split a draw runs (TASKS.md #245).
//
// Every baked draw sorts its batches into opaque and blended and runs the
// opaque ones first, because the batch order in the file is a material order
// and says nothing about depth: a blended batch drawn before the opaque
// surface behind it would blend against an unfinished frame. Inside one model
// that is enough. Across the world it is not -- a section is one model, and a
// neon race barrier blended at the end of section A is still drawn before the
// buildings of section B that stand in front of it, so the barrier shows
// through them.
//
// The phases let the caller run the split over the whole draw list instead of
// per model: every visible section's opaque geometry, then every visible
// section's blended geometry. ALL is the single-model behaviour and is what
// everything but world_draw() uses.
typedef enum {
	MODEL3D_PHASE_ALL = 0,
	MODEL3D_PHASE_OPAQUE,
	MODEL3D_PHASE_BLENDED,
} Model3DPhase;

void model3d_draw_scene_lod_phase(const Model3D* m, const C3D_Mtx* vp,
	float ambient, float gain, float invGamma, bool farLod, Model3DPhase phase);

// True when the model has anything for the blended phase at all, so a caller
// running the two phases can skip the second visit for the sections that are
// wholly opaque. False while the no-blend debug switch is on, for the same
// reason the draw itself stops blending then.
bool model3d_has_blended(const Model3D* m);

// True when the blob actually carries a far-detail run. Only for reporting --
// drawing does not need to ask.
bool model3d_has_lod(const Model3D* m);

// True when some instance of the section is in a car's reflection (#166).
bool model3d_has_env(const Model3D* m);
// In an env view (model3d_set_env_view): works out the model's instance picks
// for the face being drawn, if it has not yet, and says whether any of them is
// drawn -- so a caller can skip a section with nothing in this face before
// binding anything for it.
bool model3d_env_any(const Model3D* m);

// ------------------------------------------------ texture UV scroll (#269)
//
// Retail scrolls a texture whose TPK record sets a scrollType: water, the
// race barriers' chevrons, light strings, rolling neon. eDataRender::Render
// (Speed.elf 0x8031164C) asks TextureInfo::GetScroll (0x800CFDAC) for an S
// and a T off WorldTimer and loads them as a texture-matrix translation. A
// world texture that scrolls carries the type in its material (bits 8-9,
// tools/track_pack.py world_material) and the rest in the bank record;
// world.c hands each one over here when the bank opens.
//
// `timeStep` .. `offsetT` are the record's raw s16s (see
// tools/extract_global_textures.py for their units).
void model3d_scroll_clear(void);
void model3d_scroll_add(u32 hash, int type, s16 timeStep, s16 speedS, s16 speedT,
	s16 offsetS, s16 offsetT);
int  model3d_scroll_count(void);
// WorldTimer, in retail's 1/4000 s ticks. Set once a frame, before drawing;
// every scrolling texture's offset is worked out here, once.
void model3d_set_world_timer(u32 ticks);

// The world's texture animations (TASKS.md #326), from its bank: each is its
// frame hashes, frame 0 first, and its rate. Cleared with the bank; switched
// off by the Animated Textures setting (every texture then shows its own art).
void model3d_anim_clear(void);
void model3d_anim_add(const u32* frames, int n, int fps);
int  model3d_anim_count(void);
void model3d_anim_enable(bool on);
// Batches drawn with an animated texture, and how many of them showed another
// frame's art, since the last call (the periodic world log).
void model3d_anim_stats(u32* draws, u32* swaps);

// ------------------------------------------- sub-section frustum cull (#68)
//
// The GameCube build culls scenery per instance, through a per-section tree
// (ScenerySectionHeader::TreeCull) whose nodes carry their own boxes and whose
// visible state -- 0 outside, 1 straddling, 2 fully inside -- is inherited by
// the children of a node already fully inside. A merged batch here is far too
// coarse to stand in for a leaf, so model3d_build_cull_cells() cuts each one
// into a grid of boxed runs and the draw loop walks those with retail's three
// states. See the long note above the builder in model3d.c for the measured
// case and the constants.
//
// Sorts the model's index pool in place and builds the table, returning its
// size in bytes (0 if it was not built, which simply leaves every batch drawn
// whole). Must run before model3d_build_phys_grid(), which lists triangles by
// their position in that pool; it refuses if the grid already exists. Off the
// frame, like the grid: it is several ms on a big section.
size_t model3d_build_cull_cells(Model3D* m);

// The four side planes of the view frustum in world space, as world.c pulls
// them out of the view-projection matrix (`&fr.p[0]`). Set once a frame before
// drawing world sections and cleared with NULL after: anything drawn through a
// model matrix of its own -- the car, the skydome, the shadow, a frontend
// room -- has boxes in model space and must not be tested against these.
void model3d_set_cull_planes(const float planes[4][4]);

// Cells built and the grid square they settled on, for the load report.
void model3d_cull_info(const Model3D* m, int* outCells, float* outCell);

// ------------------------------------------- per-instance scenery LOD (#247)
//
// Retail picks every scenery instance's level of detail each frame from its
// size on screen (Speed.elf DrawAScenery 0x80208fa4; see model3d.c). A world
// section packed with an instance table has the instances' records and the
// index runs each one draws, and model3d_build_cull_cells() turns them into
// that model's LOD and cull tables in place of the grid cells.
//
// The view the pick is made from: the camera position, its unit forward
// vector, and H = 256 / tan(fov / 2) (CalculateH 0x801d8bf8). Set once a frame
// before world sections are drawn; NULL draws every instance at its A level,
// which is the near-detail debug switch.
void model3d_set_lod_view(const float eye[3], const float fwd[3], float H);
// g_WorldLodLevel (#326): below 2 the player's view draws every instance at
// its B level, as SPEED2.EXE's pick does (model3d.c).
void model3d_set_world_lod(int level);

// While on, world sections draw as an env-map face sees them (#166): the
// instances DrawAScenery's env pass would draw, at their reflection models,
// picked from the LOD view above (the face camera, H 256). Only sections with
// an instance table draw anything.
void model3d_set_env_view(bool on);
// The env pick's P threshold: 0 is the 3DS's 64 (#432); 32 the GameCube's.
void model3d_set_env_pmin(int p);
// The env pick's distance half worked out once per eye for all its faces
// (#432, on by default); the result is the same either way.
void model3d_set_env_pick_once(bool on);

// ------------------------------------------ the projected headlight (#296)
//
// SPEED2.EXE's WORLDHEADLIGHT: the world pieces inside the player's headlight
// frustum are drawn with worldheadlight.efx instead of the world effect,
//
//   rgb = sat(2 * diffuse * (HEADLIGHTS * clip * HeadlightColor + colour))
//
// HEADLIGHTS projected from a camera over the car's nose and the clip a ramp
// along the beam -- the GameCube's eTagHeadlightCallback / flag 0x200 is the
// same thing. Here the batches whose box meets the textured frustum go
// through scenehl.v.pica and four TEV stages; outside it the beam is black,
// so every other batch keeps the plain path and its pixels are unchanged.
// carlights_headlight() fills this in.
typedef struct {
	float rows[4][4];     // scenehl.v.pica's hl[]: s*w, t*w, clip, w, world space
	float planes[6][4];   // the textured frustum, inside >= 0, world space
	float corners[8][3];  // its corners (#412): bit 0 x+, bit 1 y+, bit 2 the far end
	u32   colour;       // HeadlightColor / 2 as a TEV constant (0xAABBGGRR)
	C3D_Tex* beam;        // HEADLIGHTS, loaded as GPU_TEX_PROJECTION
	C3D_Tex* clip;        // the clip ramp, 2D
} Model3DHeadlight;

// For the world sections' draws in between, and only those: the sky, the
// props, the cars, an env view and anything drawn through a matrix of its own
// never take it. NULL turns it off. False when scenehl.v.pica would not load.
bool model3d_set_headlight(const Model3DHeadlight* hl);
// Batches drawn with the beam and program switches since the last
// model3d_reset_draw_stats().
void model3d_headlight_stats(u32* outBatches, u32* outSwitches);
// The triangles the beam was laid over this frame (#412).
u32 model3d_headlight_tris(void);
// 1 cuts the beam's runs and cells to its frustum (#412); DbgBeamRunCull=0 is the A/B.
extern u32 g_hlRunCull;
// #412: between these the world's beam is queued over every section and laid
// over them in one go at the flush (world_draw, after the opaque phase).
// DbgBeamDefer=0 (g_hlDefer) lays it per section again.
void model3d_beam_defer(bool on);
// `vp` is the world-space view-projection the sections drew with: the queued
// beam is scissored to its frustum's box on screen (#412).
void model3d_beam_flush(const C3D_Mtx* vp);
extern u32 g_hlDefer;
// #412 one pass: 1 draws an opaque, dry, untinted batch's pieces in the beam
// once, through scenehl's whole sum, at the flush, instead of plain and then
// the additive overlay (config DbgBeamOnePass=0 for the A/B).
// model3d_beam_two_pass(true) is the look sweep's beam2pass step;
// model3d_headlight_full() is how many of the frame's beam batches went one
// pass.
extern u32 g_hlOnePass;
extern u32 g_blendCut;   // #173: alpha test GREATER 0 on blended batches
void model3d_beam_two_pass(bool on);
u32 model3d_headlight_full(void);
// #412: 1 scissors the deferred beam to its frustum's box on screen (config
// DbgBeamScissor=0 for the A/B). The look sweep's steps:
// model3d_beam_scissor_mode(0 off / 1 the box / 2 an 8x8 corner, the beam's
// vertex work without its pixels; `baseTiny` when the world's whole pass is
// already in the sweep's 8x8 corner, which the flush puts back).
extern u32 g_hlScissor;
void model3d_beam_scissor_mode(int mode, bool baseTiny);
// This frame's beam box in framebuffer pixels (l, t, r, b) and its share of
// the target, for the pager line; all zero when no box was set.
void model3d_beam_scissor_stats(u16 outRect[4], float* outShare);

// ----------------------------------------- the road's reflection (#296)
//
// eDisplayFrame's views 4 and 5: the player camera's own view of the scenery
// with every prop mirrored about its own origin (DrawAScenery's 0x800 mask,
// which negates the instance matrix's [10]), the player's car flipped about
// its own floor, and the cars' flares, captured to a texture
// (reflection.c). The reflective world batches -- sub-mesh flag 0x40, the
// wet road -- then take it at their own screen position, ps_Model(2):
//
//   rgb = (2 - a) * tex * ras + a * capture,   a = tex.a / 2
//
// which on the port's plain pixel P = tex * colour is P (1 - tex.a/4) +
// capture * tex.a/2.
//
// While a reflection view is set, world sections draw as that view sees them:
// the env set (as model3d_set_env_view, picked from the LOD view) less the
// terrain, each instance through the player's `vp` times its mirror. Only
// sections packed with the NFSR table draw anything. NULL ends it.
void model3d_set_refl_view(const C3D_Mtx* vp);
// PORT (TASKS.md #376-#378, #382, #387): the world's sections are in world
// space, a kilometre from the origin, which leaves the 3DS shader's 24-bit
// floats about a centimetre of depth that shifts as the camera moves --
// surfaces that close took turns in front, frame to frame, on hardware.
// While set, the baked draws (drawBakedLod) take positions relative to `eye`
// rounded to whole metres, the same clip position computed on small numbers.
// Only for models in world space (the world's sections); NULL ends it.
void model3d_set_world_origin(const float eye[3]);
// The capture for the reflective batches of the world draws that follow,
// in place of their plain path; NULL draws them plain. False when its program
// would not load. The texture must be GPU_TEX_PROJECTION: only unit 0 can
// sample it, at the pixel's own place on screen.
bool model3d_set_reflection(const C3D_Tex* capture);
// The road's dampness, 0..1 (#315, rain_dampness): how much of the capture
// the reflective batches take, KColorWorldReflection's alpha. 1 by default.
void model3d_set_wet_damp(float d);
// Reflective batches drawn with it and the instance mirrors uploaded since
// the last model3d_reset_draw_stats().
void model3d_reflection_stats(u32* outBatches, u32* outMirrors);
// #328: the world draws that follow log each batch they draw, in order, as
// "batch dump" lines -- its texture, the program and passes it took and where
// its box lands on the top screen -- up to `lines` of them. 0 stops it.
void model3d_batch_dump(int lines);
// g_TextureFiltering (#326): bilinear, or point, for the textures of every
// model built from now on (the cars, the props, the rooms). The street's bank
// textures are world_set_tex_filter()'s.
void model3d_set_tex_filter(bool linear);
// #328's screen probe: how a reflective batch in the beam gets its capture.
// 0 as built; 1 not at all (the beam alone); 2 with the fog left on at colour
// 0 rather than turned off and on again; 3 after a framebuffer flush.
void model3d_set_wet_probe(int mode);
// #395: true draws the beam the pre-#395 way, each batch in it through
// scenehl.v.pica in place of its plain draw (DbgNoBeam=3); false (default)
// lays it additively over the plain draw.
void model3d_set_beam_inline(bool on);

// Instances drawn at A and at B by the world draw since the last
// model3d_reset_draw_stats(), for the status overlay.
void model3d_lod_stats(u32* outA, u32* outB);

// The instance table built for the load report: instances, runs, and the
// boxed pieces the large runs were cut into. Zeros for a model with none.
void model3d_inst_info(const Model3D* m, int* outInsts, int* outRuns,
	int* outBoxes);

// The smokeable props (smokeable.c, TASKS.md #318). Retail draws a prop a car
// has knocked loose through the Smokeable that replaced it and hides its
// scenery instance (SceneryOverrideInfo flag 0x10); here the instance is the
// one in the section's instance table at the spawner's translation.
//
// The instance whose translation is `pos` (within 1 cm). False with no table.
bool model3d_find_instance(const Model3D* m, const float pos[3], u32* outInst);
// Takes the instance out of the model's own draw, both views, or puts it back.
void model3d_hide_instance(Model3D* m, u32 inst, bool hide);
// The light flares the props carry (carlights.h, TASKS.md #323), which
// DrawAScenery takes off the model it draws an instance at. The frame number
// the instance states are worked out for -- bumped by each new view -- and
// which model the player's view drew instance `inst` of `m` at in frame
// `frame`: 1 its A, 2 its B, 0 neither, or the model was not drawn then.
u32  model3d_lod_frame(void);
int  model3d_instance_lod(const Model3D* m, u32 inst, u32 frame);
// The batches drawing instance `inst`, a bit each in `mask`, which holds
// (model3d_batch_count(m) + 7) / 8 bytes. False when none does.
int  model3d_batch_count(const Model3D* m);
bool model3d_instance_batches(const Model3D* m, u32 inst, u8* mask);
// Draws that instance alone under `mvp` -- the view-projection times where it
// has moved to from where it was baked -- at the level of detail it takes at
// `pos`, its translation now. Unculled; nothing in an env view.
void model3d_draw_instance(const Model3D* m, u32 inst, const u8* mask,
	const float pos[3], const C3D_Mtx* mvp, float ambient, float gain,
	float invGamma);

// Draw calls and triangles actually submitted since the last reset. The drawn
// section count alone cannot show what the cull did -- a section cut down to
// two cells and one drawn whole both count as one section.
void model3d_reset_draw_stats(void);
void model3d_draw_stats(u32* outCalls, u32* outTris);

// The CPU split (#345): with it on, the ticks spent in the per-instance LOD and
// cull and inside C3D_DrawElements are summed; model3d_cpu_split() reads and
// clears them.
void model3d_set_cpu_split(bool on);
void model3d_cpu_split(u64* outInst, u64* outEmit);
// Draws timed, those right after a program change, and their ticks.
void model3d_cpu_split_counts(u32* outDraws, u32* outSwitches, u64* outSwitchTicks);
// What the batch walk did since the last read (#352), read and cleared.
enum {
	MODEL3D_WALK_LOOKED, MODEL3D_WALK_CULLED, MODEL3D_WALK_DRAWN,
	MODEL3D_WALK_RUNS, MODEL3D_WALK_RUNBOX, MODEL3D_WALK_TEV, MODEL3D_WALK_COUNT
};
void model3d_walk_stats(u32 out[MODEL3D_WALK_COUNT]);

// ------------------------------------------------------------- other passes
//
// The world draws three kinds of baked-lighting geometry through the one
// scene shader, and they differ only in their depth and blend rules:
//
//   WORLD  the streets themselves -- depth tested, depth writing, alpha
//          cutout on the fence/foliage/decal art, fogged.
//   SKY    the skydome (TASKS.md #148). No depth test and no depth write, so
//          it can be drawn first at whatever radius suits the far plane and
//          have the whole world painted over it; blended rather than cutout,
//          because the cap texture's outer ring is a soft alpha fade into the
//          horizon band and a 0x40 cutout would turn that into a hard circle;
//          and unfogged, because it *is* the thing the fog fades into.
//   DECAL  the car shadow. Depth tested so the road hides it where the road
//          is in front, but not depth writing, and blended.
//
// `mvp` is the whole transform, not just the view-projection: a section's
// vertices are already in world space, but the skydome is a unit dome that
// has to be scaled and moved onto the camera and the shadow is a unit quad
// that has to be put under the car.
typedef enum {
	MODEL3D_PASS_WORLD,
	MODEL3D_PASS_SKY,
	MODEL3D_PASS_DECAL,
} Model3DPass;

void model3d_draw_baked(const Model3D* m, const C3D_Mtx* mvp, float ambient,
	float gain, float invGamma, Model3DPass pass);

// ------------------------------------------------------------ distance fog
//
// The PICA200's fixed-function fog unit, which is what stops DRAW_RADIUS from
// being a hard edge into black (TASKS.md #148). `rgb` is 0xRRGGBB and wants to
// be the skydome's horizon colour; `density` and `gradient` are the terms of
// exp(-(density*z)^gradient); `nearZ`/`farZ` must match the projection the
// geometry is drawn with, because the fog LUT is indexed by window depth and
// those are what linearise it.
//
// Setting fog does not turn it on. It stays off until model3d_fog_enable(true)
// and has to be turned off again before anything that must not be fogged --
// the 2D passes above all, which draw at a window depth the fog curve reads as
// the far plane and would come out solid fog colour.
void model3d_fog_set(u32 rgb, float density, float gradient, float nearZ, float farZ);
void model3d_fog_enable(bool on);
// For a pass outside this module that binds its own program and quad
// (trails.c, smear.c, bleach.c): call once they are bound, before the first
// real draw. The fog goes off again and a zero-area draw takes the mode and
// program change (#486, after #400 and #416).
void model3d_post_pass_prime(void);
// A multiplier on the fog's visibility (1 = clear) per eye depth z, laid over
// the curve model3d_fog_set builds; NULL for none (#407, the rain's share).
// `ctx` must outlive the shape. The LUT goes up on the next fog enable.
void model3d_fog_shape(float (*mul)(float z, const void* ctx), const void* ctx);

// Draws a lit car standing on the world plane at `pos` -- the point its wheels
// touch, which is *not* the model's own origin: the lift from the origin down
// to the contact plane is applied inside -- through the same camera `vp` the
// room was drawn with.
//
// `yaw` is about world +Z, `pitch` is nose-up positive and `roll` is
// right-side-down positive, matching the CarSim pose fields. All three pivot
// about the contact point, so a car on a cambered road leans into the camber
// with its tyres still on it instead of swinging its nose through the tarmac.
// Pass 0 for pitch and roll to stand the car flat, which is what the frontend
// turntable wants. A car with light materials is lit as the world's are (see
// model3d_set_light_mats()) once model3d_set_car_view() has been told where
// this camera is, by the rig model3d_set_car_rig() last set.
void model3d_draw_car(const Model3D* m, const C3D_Mtx* vp, const float pos[3],
	float yaw, float pitch, float roll);

// The same car placed by a rigid body instead (TASKS.md #222): `rot` has the
// car's forward, left and up axes in world space as rows, and `origin` is
// where the record origin -- the model's own origin -- is in the world. No
// lift is applied; the physics already has the wheels on the road.
void model3d_draw_car_xform(const Model3D* m, const C3D_Mtx* vp, const float rot[3][3],
	const float origin[3]);

// The body colour every car is drawn with, as 0xRRGGBB. Set it from the
// player's profile -- the save's BASE_PAINT slot names a part and PAINT.nfsp
// turns that into RGB (tools/paint_pack.py). The body is the composited skin
// (carskin.c), which carries its colour already; this shades the car batches
// that have no texture at all. Until something sets it, they wear NFSU2's own
// promo blue.
void model3d_set_car_paint(u32 rgb);
// Also clears the component paints below.
void model3d_reset_car_paint(void);

// An assembled career car (carkit.c) tags each batch with the paint material
// it takes: the body, or one of the spoiler, roof scoop, mirror, rim,
// exhaust, brake and audio materials Render__13CarRenderInfo keeps beside it
// (carkit.h CARKIT_PAINT_*, same order). This sets those, 0xRRGGBB each, for
// the slots whose `set` entry is true; the rest, and [0], follow the body.
// NULL for either clears them all.
#define MODEL3D_CAR_PAINT_SLOTS 8
void model3d_set_car_slot_paints(const u32* rgb, const bool* set);
// The same two for one model alone, which then ignores the globals: an AI car
// (opponent.c) wears its own colours while the player's car wears the
// profile's. `body` is 0xRRGGBB, or anything above 0xFFFFFF for the fallback
// blue; `rgb`/`set` as above.
void model3d_set_model_paints(Model3D* m, u32 body, const u32* rgb, const bool* set);
// Batch flag beside that slot: the batch is textured, and Render repoints its
// light material at the slot's paint (a rim's MAGSILVER/MAGCHROME/MAGGUNMETAL
// sub-meshes, a brake caliper), so the texture is multiplied by the paint as
// well as lit. carkit.c sets it only where retail makes that swap; a slot with
// no paint set draws the batch as plain texture.
#define MODEL3D_BATCH_PAINT_TINT 0x10000000u

// Batch flags bits 16-19 on a car carkit.c assembled (TASKS.md #296): the car
// LODs (bit 0 = A .. bit 3 = D) the batch is drawn at, or 0 for every level.
// CarRenderInfo::Render draws each part at the level it picks for the car
// (world_car_lod), out of the part's own LOD models, and leaves out a part
// with no model at that level. The bits are the scenery-set field's on a
// world section; model3d.c only reads them as LODs on a model
// model3d_set_car_lods() was called on.
#define MODEL3D_BATCH_LOD_SHIFT 16
#define MODEL3D_BATCH_LOD_MASK  0x000F0000u
#define MODEL3D_CAR_LODS        4

// Batch flags bits 20-23 on a car carkit.c assembled or a traffic pack
// (car_pack.py BATCH_CORNER_SHIFT; TASKS.md #296): the wheel part a batch
// is, at any of its LODs. Bits 22-23 the kind -- MODEL3D_WHEEL_RIM (the wheel
// part's model 0, or a traffic car's tyre), _BRAKE or _SPINNER -- and bits
// 20-21 its corner, 0 FL, 1 FR, 2 RR, 3 RL (carkit.h CarKitInfo rim); 0 on
// every other batch, whose corner bits mean nothing then.
// Those batches move with their corner (model3d_set_car_wheels). And in game,
// at the car's LOD A, CarRenderInfo::Render (Speed.elf 0x8023d548 /
// 0x8023d6f8, SPEED2.EXE 0x62b628 / 0x62cff5) draws a rear rim whose inner
// side TireFace turns to the camera with the part's LOD B model
// (CarRenderInfo +0x1b60, PC +0x1b6c) in place of its A one.
#define MODEL3D_BATCH_CORNER_SHIFT 20
#define MODEL3D_BATCH_CORNER_MASK  0x00300000u
#define MODEL3D_BATCH_WHEEL_SHIFT  22
#define MODEL3D_BATCH_WHEEL_MASK   0x00C00000u
#define MODEL3D_WHEEL_RIM     1
#define MODEL3D_WHEEL_BRAKE   2
#define MODEL3D_WHEEL_SPINNER 3
// Where each corner's wheel parts turn about, in model space: the wheel
// record's x and z, and y on the tyre's centre plane -- Render's frame
// (SPEED2.EXE 0x620ae0) sets an aftermarket rim half its width outboard of
// it, a stock one on it. NULL clears them, and the wheels are drawn as baked.
void model3d_set_car_pivots(Model3D* m, const float pivot[4][3]);
// Those pivots back, FL FR RR RL; false when the model has none.
bool model3d_car_pivots(const Model3D* m, float out[4][3]);
// The wheels' motion for the next model3d_draw_car* call, as
// CarState::CalculateWheelMatrices (Speed.elf 0x801aa974) and Render's camber
// make it: each corner's batches turn about its pivot -- a rim by
// Rx(camber) Rz(steer) Ry(spin), a brake the same without the spin, a
// spinner by its own angle in the spin's place -- and rise by the
// suspension. Steering and camber are the front corners' alone, and the
// camber only at LOD A and B, + on FL and - on FR. That call goes back to
// parked wheels after it, as it does to LOD A.
typedef struct {
	float spin[4];      // rad, about the axle; positive rolls the top forward
	float spinner[4];   // rad, the spinners' own (Car +0x4E8)
	float susp[4];      // m, up
	float steer;        // rad, positive to the left
	float camber;       // rad, positive tilts the tops inboard
} Model3DWheels;
void model3d_set_car_wheels(const Model3DWheels* w);

// The car's opening parts (TASKS.md #372 step 7): CarRenderInfo::Render
// (Speed.elf 0x80239018) asks GetCarSlotAnim (0x802442cc, GLOBALB.BUN chunk
// 0x34608) which animation location each slot's model 0 / 1 belongs to -- 0
// the hood, 1 the trunk, 2 the left door, 3 the right one -- and draws the
// models of a location that is not closed by its animation matrix. It also
// shows the door panels and sills only with their door open or in the
// frontend, and the hood / trunk undersides only with them open (the trunk's
// in the frontend too on the cars ShowTrunkUnderInFE (0x802313ec) names).
// carkit.c tags the batches; carpartanim.c works out the matrices.
//
// Batch flags with MODEL3D_BATCH_PART set: bits 20-21 the location (the
// wheel bits, which such a batch has none of), bit 22 the hood's model 1
// (a split hood's second half, which moves on its own), bit 23 a part that
// does not move (the sills), bit 27 a part shown only as said above. On a
// conditional trunk batch bit 22 instead marks the frontend showing it.
#define MODEL3D_BATCH_PART      0x20000000u
#define MODEL3D_BATCH_PART_SUB  0x00400000u
#define MODEL3D_BATCH_PART_STILL 0x00800000u
#define MODEL3D_BATCH_PART_COND 0x08000000u
// The engine bay and the trunk audio install (TASKS.md #314), as Render's
// blocks after the main loop draw them (Speed.elf render 0x8023df44 ..
// 0x8023e660): PART batches at location 0 (the engine: COND, shown with the
// hood not closed) or 1 (the trunk box and its components), all STILL --
// authored in car space. Bits 30-31 are the room / section flags
// (BATCH_TURNTABLE, BATCH_NO_COLLIDE in model3d.c), which no car reads:
//   PART_LIT: lit by the car's light scaled by its location's animT, x0.6
//     for the hood and x0.9 for the trunk -- ENGINE_A, TRUNK_AUDIO_A and the
//     components; their _UNLIT_A models take the light as it is. carsh only.
//   PART_AUDIO: a component. With PART_SUB it is the installed model, drawn
//     in a frontend draw; without, the AUDIO_COMP_EMPTY placeholder the
//     world draws in its place.
#define MODEL3D_BATCH_PART_LIT   0x40000000u
#define MODEL3D_BATCH_PART_AUDIO 0x80000000u
#define MODEL3D_PART_LOCS   4
#define MODEL3D_PART_FRAMES 5   // hood 0, hood 1, trunk, left door, right door
// For the next model3d_draw_car* call only, as the wheels are: frame f moves
// its batches by p' = rot p + t (column vectors, model space). A draw without
// one has every part shut and none of the conditional ones drawn.
typedef struct {
	bool fe;                         // a frontend draw
	bool open[MODEL3D_PART_LOCS];    // the location is not closed
	bool moved[MODEL3D_PART_FRAMES];
	float rot[MODEL3D_PART_FRAMES][3][3];
	float t[MODEL3D_PART_FRAMES][3];
	// GetAnimationTime: the location's swing as 0..1 (1 open), 0 closed.
	float animT[MODEL3D_PART_LOCS];
	// The level-3 bay neon (TASKS.md #506): the hood's [0] and the trunk's [1]
	// pulsed neon colour as Render adds it to its scaled light's constant
	// term -- r, g, b each x (1/256) x (alpha/256) (carneon_bay_light()) --
	// before the animT x0.6 (hood) or x1.5 (trunk) it is scaled by here.
	float neonAdd[2][3];
	// RideInfo::UpdatePartsEnabled's FRONT_WHEEL (slot 0x1D) off while the
	// paint shop browses brake paint (IsBrowsingBrakePaint, Speed.elf
	// 0x80244EE8 / SPEED2.EXE 0x61BCD0; #517): the front corners' rims are not
	// drawn, so the calipers show.
	bool noFrontWheel;
} Model3DPartPose;
void model3d_set_car_parts(const Model3DPartPose* p);
// Marks `m` as a car with MODEL3D_BATCH_PART_LIT batches (carkit.c), whose
// draws put up the scaled light for them.
void model3d_set_car_bay(Model3D* m, bool on);
// The two rear rims' local +y in car space (row 1 of CarKitInfo rim[2] and
// rim[3]: it runs inboard), for that test. model3d_draw_car_xform() turns each
// by the car's rotation and dots it with model3d_set_car_view()'s forward; at
// or above FaceCos (0) the rim keeps its A model. NULL clears them.
void model3d_set_car_rims(Model3D* m, const float axis[2][3]);
// Whether the car drawn at rot / origin shows its underside to the camera
// model3d_set_car_view() set: CarPartCuller's UNDERNEATH part, as
// CarRenderInfo::Render culls it. False without the view or the pivots.
bool model3d_car_underside_seen(const Model3D* m, const float rot[3][3],
	const float origin[3]);

// The car's glass (TASKS.md #324). UpdateCarReplacementTextures (Speed.elf
// 0x802353FC, SPEED2.EXE 0x615280) points each of the six window slots --
// front, rear, left front, left rear, right front, right rear -- at
// DamagedWindowHashes[level]: WINDOW at 0, WINDOW_DAMAGE0..2 at 1..3, and the
// rear window's REAR_DEFROSTER at the rear's level once it is 1 or more.
// carkit.c names the batches that are glass (`batch`, before
// model3d_merge_batches, which keeps each apart and follows it) and the
// window each is (`win`), and the model's three cracked-pane textures. False
// when any of it is out of range.
#define MODEL3D_GLASS_WINDOWS 6
bool model3d_set_glass(Model3D* m, const int* batch, const u8* win, int n, const s32 tex[3]);
int model3d_glass_count(const Model3D* m);
// Each window's level, 0..3: its batches take the matching texture. Cheap
// when nothing changed.
void model3d_set_glass_levels(Model3D* m, const u8 level[MODEL3D_GLASS_WINDOWS]);

// Marks `m` as a car whose batches carry MODEL3D_BATCH_LOD_* (carkit.c).
void model3d_set_car_lods(Model3D* m, bool on);
// The LOD (0 = A .. 3 = D) the next model3d_draw_car* call draws a car at;
// that call puts it back to A, so every other car draw -- the frontend's
// turntables and showrooms -- is A as before.
void model3d_set_car_lod(int lod);

// Each batch's light material (TASKS.md #166): a lightmat_index() per batch,
// in file order, which carkit.c picks the way CarRenderInfo::Render does. A
// model given them is lit by model3d_draw_car_xform() the way the GameCube
// lights a car in the world -- per vertex, out of the CarsInGame rig folded
// into spherical harmonics, with a specular pass over the texture's gloss --
// once model3d_set_car_view() has said where the camera is. `n` must be the
// model's batch count; the table is copied. False if it was not taken (a
// count that does not match, no memory, or a shader that would not load), and
// the model keeps the plain lighting.
bool model3d_set_light_mats(Model3D* m, const u8* mats, int n);
// #345: draws runs of alike neighbouring batches as one call; returns how
// many batches it took away (0 when the model is not one it can touch).
int  model3d_merge_batches(Model3D* m);
// The world camera, for that light: its position, and its right, up and
// forward axes in world space, unit length. The view sets it once a frame.
void model3d_set_car_view(const float eye[3], const float right[3],
	const float up[3], const float fwd[3]);
// One light of a shaper rig (eShaperLightRig, chunk 0x3B700 in FrontB.lzc):
// its records 1-3, record 0 being read by nothing. `camera` is the record's
// flag -- 1 turns with the camera, 0 stands in the world -- then azimuth and
// elevation in degrees, the colour and its intensity.
typedef struct {
	u32 camera;
	float az, el;
	float rgb[3];
	float intensity;
} Model3DShaperLight;
#define MODEL3D_RIG_LIGHTS 3
// The rig those cars are lit by (elSetupLights' second argument), copied, and
// whether it is the frontend's: elSetupEnvMap lets the eye stand up to 8 units
// off the car there (IN_FRONTEND) rather than 2.78. NULL puts back
// ShaperLightsCarsInGame, the world's.
void model3d_set_car_rig(const Model3DShaperLight* rig, bool frontEnd);
// The lights elSetupLights folds into that light beside the rig: the street
// lamps and the other cars' lamps QueryLightDatabase finds around the car
// (lightdb.h). The query is handed the car's position and fills `out` with up
// to `max` of them, each as where it is and its colour already weighted, in
// 0..255 units (lightdb_hit_colour()); it returns how many. NULL (the
// default) lights a car by the rig alone.
typedef struct {
	float pos[3];
	float rgb[3];
} Model3DCarLight;
#define MODEL3D_CAR_LIGHTS 24
typedef int (*Model3DCarLightQuery)(const float pos[3], Model3DCarLight* out, int max,
	void* ud);
void model3d_set_car_light_query(Model3DCarLightQuery fn, void* ud);
// The reflection those cars add on their env-mapped batches (envmap.h): the
// sphere map, and the axes of the camera it was built for -- right, up, and
// back toward the world camera -- in world space, unit length. NULL (the
// default, and the frontend's) leaves the reflection out.
void model3d_set_car_env(const C3D_Tex* sphere, const float right[3], const float up[3],
	const float back[3]);

// Uploads a car skin (carskin.c: 0xAARRGGBB texels, row 0 the top) into every
// TEXFMT_SKIN slot of `m` -- the <CAR>_SKIN1 placeholder tools/kit_pack.py
// leaves in a car's kit -- with its whole mip chain. A skin larger than the
// slot is box-halved to it. `argb` is used as scratch and left halved. False
// when `m` has no skin slot, or the sizes cannot be matched.
bool model3d_set_skin(Model3D* m, u32* argb, int w, int h);
// The textures a model load could not allocate since boot, and their KB
// (#413): each draws its batches untextured.
void model3d_tex_alloc_fails(u32* count, u32* kb);
// TASKS.md #436: while on, a model built from memory takes any texture whose
// pixels match one a live shared model already holds (the same car twice,
// the kits every car uses) instead of uploading its own copy; the pixels are
// refcounted and freed with their last model. carkit_assemble() turns it on
// around its build. A shared texture written later (model3d_set_texture) is
// first given back for a private copy. Skins are never shared.
void model3d_share_textures(bool on);
// Since boot: textures taken from the share instead of uploaded, and their KB;
// and what the share holds now.
void model3d_share_stats(u32* hits, u32* hitKB, u32* liveTex, u32* liveKB);

// Writes 0xAARRGGBB texels (row 0 the top) over `m`'s own texture `hash`, in
// whatever format that slot was packed in, with its whole mip chain: what
// retail's texture swap does when it hands a mesh a composited DUMMY_WHEELn
// or DUMMY_SPINNERn (carskin_wheels()). Larger art is box-halved to the slot;
// `argb` is used as scratch and left halved. False when `m` has no such slot
// (or draws a shared bank's textures), or the sizes cannot be matched.
bool model3d_set_texture(Model3D* m, u32 hash, u32* argb, int w, int h);

// Fallback for when there is no room to stand the car in: draws it alone,
// normalised and self-framed to fill the current render target, spun `angle`
// radians about its own vertical axis. `aspect` is the target's width/height.
void model3d_draw_car_solo(const Model3D* m, float angle, float aspect);

// ------------------------------------------------- shared-texture-bank hooks
//
// A world section's .nfsc carries a texture table but no pixels: they live in
// one WORLD.NFSB bank shared by every section, because L4RA's sections
// reference 1562 distinct textures 20286 times between them and embedding
// meant carrying each one a dozen times over on the SD card -- and, worse,
// re-uploading art on every page-in that a neighbouring resident section
// already had in VRAM. So the loader records the hashes, and the pager
// (world.c) resolves them against the bank and hands the result back.

// How many texture slots the blob declares (batch texIndex values index this).
int model3d_texture_count(const Model3D* m);

// The bStringHash of the texture in `slot`, or 0 if there is none.
u32 model3d_texture_hash(const Model3D* m, int slot);

// The pixels the blob carried for `slot` itself, NULL where it had none: what
// a caller building a model3d_set_textures() array from the model's own
// textures fills it with (a traffic car's skins, traffic.c).
C3D_Tex* model3d_own_texture(Model3D* m, int slot);

// Surface-identification diagnostics; see model3d.c. Both off by default.
// g_dbgOnlyHash draws only batches using that texture hash (0 = all);
// g_dbgHashTint flat-shades every batch by the top three bytes of its hash.
extern u32  g_dbgOnlyHash;
extern bool g_dbgHashTint;
// g_dbgNoBlend skips the blended (transparent/additive) pass entirely.
extern bool g_dbgNoBlend;
// g_dbgCarPlainLight draws the cars with the old single-light shader
// (car.v.pica) even when they carry light materials (#166), the frontend's
// too.
extern bool g_dbgCarPlainLight;
// g_dbgNoRimSwap: 1 keeps every rear rim at its A model (no TireFace swap,
// model3d_set_car_rims), 2 swaps only every other second, 3 swaps both rear
// rims whichever way they face, to show the B models in place.
extern int g_dbgNoRimSwap;
// 1 turns model3d_merge_batches() off (DbgNoMerge).
extern int g_dbgNoMerge;
// 1 keeps the merge to neighbours in draw order (DbgNoMergeSort, #352).
extern int g_dbgNoMergeSort;
// #352 merge stats: opaque batches, groups, states, without LOD, without retail bits.
extern int g_mergeStat[7];
// DbgNoPassSkip=1: the car's gloss and env passes draw every batch, even one
// whose light material adds nothing there (#352).
extern int g_dbgNoPassSkip;
// DbgNoGlossEnv=1: the car's gloss and reflection as two passes over the
// opaque batches again, not carge.v.pica's one (#431).
extern int g_dbgNoGlossEnv;
// The look sweep's carsplit step (#431): DbgNoGlossEnv for its window.
extern bool g_sweepGlossEnvSplit;
// DbgTevKeep (#352, off since #350): 0 every texture change rebuilds the TEV
// stage; 1 only unit 0 is rebound while the plain stage stays; 2 as 1 with
// stage 0 re-emitted.
extern int g_tevKeep;
// DbgForce (#350's leftover), a bit mask: what every drawn world batch writes
// again even when the pass believes it is already set. 1 the texture, TEV
// stage and cutout test; 2 the UV scroll uniform; 4 the blend, depth and fog
// colour; 8 every vertex shader uniform; 16 a batch partly in view drawn
// whole instead of by its cells.
#define DBG_FORCE_TEV   1
#define DBG_FORCE_UV    2
#define DBG_FORCE_BLEND 4
#define DBG_FORCE_UNIF  8
#define DBG_FORCE_WHOLE 16
extern int g_dbgForce;
// How many of a car's batches the gloss pass / the env pass leave out (#352).
void model3d_dead_passes(const Model3D* m, int* gloss, int* env);

// Point the model's texture slots at an externally owned array of
// `model3d_texture_count(m)` C3D_Tex pointers (NULL entries draw untextured).
// The array is borrowed, not copied: it must outlive the model, and it
// overrides any pixels the blob carried itself. Pass NULL to go back to those.
void model3d_set_textures(Model3D* m, C3D_Tex* const* textures);

// Constant colour that modulates the vertex colour of batches whose texture
// could not be resolved. Default 0xFFFFFFFF, i.e. the vertex colour alone.
// See world.c for why the world sets one (TASKS.md #136).
// Build a model from a .nfsc already in memory. Split out from model3d_load()
// so the world pager can do the (expensive, blocking) read on its own thread
// and this (cheap) part on the main one -- see #176.
Model3D* model3d_from_memory(const void* data, size_t size);

// ---- Loading a section without copying it (TASKS.md #226) -------------------
//
// model3d_from_memory() builds the model out of a buffer holding the whole
// file, which costs two memcpy()s of everything the GPU will read -- about
// 1.5 MB on a street tile, and most of the frame that lands on. The pager
// cannot pay that on the main thread, so it loads the way the GameCube build
// does: Speed.elf's TrackStreamer reserves a section's memory up front
// (AllocateMemory__13TrackStreamer) and DMAs the disc bundle straight into
// it, then "activates" it separately -- one section per frame -- rather than
// copying it anywhere (ActivateSection__13TrackStreamer).
//
// The split here is the same shape, with the allocation kept on the main
// thread because libctru 2.4.1's linearAlloc() takes no lock:
//
//   1. the loader thread reads NFSC_HDR bytes and calls model3d_plan(),
//      which is pure arithmetic and safe anywhere;
//   2. it reads plan.tableBytes more -- the texture and batch tables, which
//      sit contiguously between the header and the vertices;
//   3. the main thread calls model3d_from_plan(), which allocates the model
//      and both GPU pools and fills in everything but their contents;
//   4. the loader thread reads the file's remaining bytes straight into
//      model3d_vertex_dest() and model3d_index_dest().
//
// Nothing is copied twice and nothing large is touched on the main thread.
#define MODEL3D_HDR_BYTES 0x40
// One vertex in the pool model3d_vertex_dest() points at, for a caller that
// has to know what a pool of N vertices will cost before it is allocated.
#define MODEL3D_VERTEX_BYTES 32

typedef struct {
	u32 nTex, nBatch, nVert, nIdx;
	u32 oTex, oBatch, oVert, oIdx;  // byte offsets into the file
	u32 texOff, texSize, shading, lodSplit;
	u32 tableBytes;   // oVert - MODEL3D_HDR_BYTES, the two tables together
	u32 vertBytes;    // what model3d_vertex_dest() can hold
	u32 idxBytes;     // what model3d_index_dest() can hold
	// The instance table (TASKS.md #247), between the indices and texOff: 0
	// bytes for every blob that has none, which is all but a world section
	// packed since #247.
	u32 instOff;      // byte offset into the file
	u32 instBytes;    // what model3d_inst_dest() can hold
} Model3DPlan;

// Validate a .nfsc header and work out where everything in the file is.
// `fileSize` is the whole file; `hdrBytes` is how much of it `hdr` holds, at
// least MODEL3D_HDR_BYTES. False for anything this build cannot draw.
bool model3d_plan(const void* hdr, size_t hdrBytes, long fileSize,
                  Model3DPlan* p);

// Allocate the model described by `p` and fill in everything that comes from
// the header and the two tables. `headerAndTables` holds the file's first
// p->oVert bytes. The vertex and index pools come back allocated but
// uninitialised -- the caller fills them through the two accessors below, and
// the model is not drawable until it has. NULL if the file carries its own
// pixels (texSize != 0), which needs the whole blob and model3d_from_memory().
Model3D* model3d_from_plan(const Model3DPlan* p, const void* headerAndTables);

void* model3d_vertex_dest(Model3D* m);
// The vertices model3d_vertex_dest() holds (#471's GPS arrow recolour).
int model3d_vertex_count(const Model3D* m);
void* model3d_index_dest(Model3D* m);
// One of the two pools was moved (the world arena's compaction, linarena.h):
// its bytes were at `from` and are now at `to`. Repoints the pool, and the
// vertex buffer the next draw hands the GPU. Between frames only.
void model3d_relocate(Model3D* m, const void* from, void* to);
// The next draws read two streams in place of the interleaved pool: pos[3] +
// normal[3] (24 bytes a vertex) from `posNrm`, uv[2] (8 bytes) from `uv`, both
// linear memory (#354's skinned crowd, crowd.c). Between draws; the draw
// records the addresses into the command list, so each call may name new ones.
void model3d_set_streams(Model3D* m, const void* posNrm, const void* uv);
// Where plan.instBytes go, read from plan.instOff; NULL when the blob has no
// instance table. model3d_build_cull_cells() turns what lands here into the
// per-instance LOD and cull tables and frees it.
void* model3d_inst_dest(Model3D* m);
// The same block as model3d_load() read it, for a blob that is not a world
// section: a traffic car's extras (tools/car_pack.py's TRFX). NULL, and
// *bytes 0, when the blob has none.
const void* model3d_extra(const Model3D* m, u32* bytes);

void model3d_set_untextured_paint(Model3D* m, u32 rgba);

// A world section's scenery-set table (scenery.h): one bit per set, drawn
// when set. Borrowed; the caller keeps it alive while the model is. Also
// keeps the tagged batches out of model3d_build_phys_grid(), so call it
// before that. NULL, the default, draws every batch and reads no set bits.
void model3d_set_scenery(Model3D* m, const u8* setOn);

// Highest drivable surface directly under (`x`,`y`) that is no higher than
// `zMax`, in the same world space the vertices are in. False when the model
// has no near-horizontal triangle over that point -- the caller is off the
// edge of this section's geometry, or standing over a hole. See the
// implementation for what counts as drivable and why walls are excluded.
bool model3d_ground_z(const Model3D* m, float x, float y, float zMax, float* outZ);

// The box the model's header gives, in its own space.
void model3d_bounds(const Model3D* m, float outMin[3], float outMax[3]);

// How much of the index pool the collision scans actually read, and how much
// the blob holds. The gap is the far-LOD run plus any signage the packer
// marked BATCH_NO_COLLIDE -- see model3d.c. Diagnostics only.
void model3d_phys_span(const Model3D* m, int* outPhys, int* outTotal);

// Buckets the triangles the ground and wall scans read into an XY grid, so a
// query visits the handful near it instead of the whole pool; the answers do
// not change. Several milliseconds on the biggest world section, which is
// why world.c runs it on its loader thread rather than inside
// model3d_from_memory(). Returns the bytes it took (malloc heap), or 0 when
// it built nothing -- too few triangles to be worth it, already built, or out
// of memory -- in which case the scans walk the pool as before. Not safe
// against a scan of the same model running concurrently.
size_t model3d_build_phys_grid(Model3D* m);

// The grid's shape, for the load log. False when the model has none.
bool model3d_phys_grid_info(const Model3D* m, int* outW, int* outH, float* outCell,
	int* outRefs);

// The same probe for up to MODEL3D_MAX_PROBES points at once, in ONE pass over
// the triangle list. A car standing on four wheels has to ask four times a
// frame, and asking separately is four full scans of a section that can carry
// twenty thousand triangles; batched, the per-triangle work -- the bounding
// box, the edge vectors, the determinant, the drivability test -- is done once
// and only the barycentric test repeats. `outHit[i]` is set for each point that
// found a surface and left alone for each that did not, so results accumulate
// across several models the way model3d_walls_near()'s do. `outN`, when not
// NULL, is 3n long and gets the unit normal of the face each hit was taken
// from, written alongside outZ[i].
#define MODEL3D_MAX_PROBES 8

void model3d_ground_z_multi(const Model3D* m, const float* xy, int n,
	float zMax, float* outZ, bool* outHit, float* outN);

// One blocking triangle, reduced to the only part of it a car-sized cylinder
// can hit: the XY segment its near-vertical face projects to, and the height
// band over which it blocks. A face steep enough to fail model3d_ground_z()'s
// drivability test is exactly the face that has to stop the car instead of
// carrying it, so the two share one threshold -- nothing is both drivable and
// solid, and nothing is neither.
//
// The band also at each end (TASKS.md #280): a topology wall is a quad whose
// top edge need not be level, and a car is stopped only where the face stands
// in its body band. 4042B's ledge face at (-1140,-1397) tops out at 15.20 m
// at one end and 15.61 m at the other; its whole-quad band put a 0.3 m lip
// across a drop the road runs flush to. zmin / zmax span both ends.
typedef struct {
	float x0, y0;
	float x1, y1;
	float zmin, zmax;
	float dist2;      // squared XY distance from the query point, see below
	float z0min, z0max;   // the band at (x0, y0)
	float z1min, z1max;   // and at (x1, y1)
} Model3DWall;

// Adds the walls of `m` whose XY segment passes within `radius` of (`x`,`y`)
// and whose height band overlaps [`zLo`,`zHi`] to `out`, which already holds
// `count` of them, and returns the new count. Never more than `max`.
//
// Appends while there is room and then keeps the *nearest* `max`: a full
// buffer drops its farthest entry for any closer candidate. Order of arrival
// is index order, which has nothing to do with where the car is, so first-come
// truncation in a densely modelled street means the wall being driven into can
// lose its place to a shopfront round the corner -- and a wall that is not in
// the buffer is a wall the car drives through. Distance is comparable across
// sections because they are all asked about the same point, so filling one
// buffer over several calls stays correct.
//
// The caller gathers once over a radius covering a whole frame's travel and
// then resolves against the small result, so this runs once per frame per
// section rather than once per collision substep.
int model3d_walls_near(const Model3D* m, float x, float y, float radius,
	float zLo, float zHi, Model3DWall* out, int count, int max);

// Turns texture unit 1 or 2 off again while it still holds `bound`, the
// texture bound there last: C3D_TexBind(unit, NULL) faults on the 3DS
// (model3d.c has why). For the passes that borrow those units (fe_post.c).
void model3d_tex_unit_off(int unit, const C3D_Tex* bound);
