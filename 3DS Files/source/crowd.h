// The race-end crowd NIS's people (TASKS.md #354).
//
// Retail's Scene_EndingNis0N scenes stand a crowd of CBasicCharacterAnimEntity
// (0x37040) round the winner's car: EAGL solids skinned to the Bip02 / Bip03
// skeletons and played by CAnimCtrl. tools/crowd_pack.py bakes a scene into a
// .crowd file (its docstring is the format); this loads one and draws it.
//
// The skinning is retail's: per bone local = qt(q, t), global = local x
// parentGlobal, S = invBind x global, a vertex v' = sum w * (v x S_bone), all
// row vectors; then the entity's placement matrix and the scene's. It is done
// on the CPU into a per-entity copy of the model's vertex pool and drawn with
// the car (lit) shader.
//
// Playback (CAnimCtrl): anim frame f = max(0, t - delay) x 30 x speed x 0.5,
// run once to the range's end (the key range, or with flags & 0x40 the
// entity's SetLoopRange), then looped inside the range when flags & 0x20 or
// held at its end (#422); between keys the quaternion
// is nlerped (shortest arc) and the translation lerped. The car tracks run at
// t x 15 frames, clamped at their last key.
#ifndef CROWD_H
#define CROWD_H

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct Crowd Crowd;

// Loads `path` (e.g. sdmc:/nfsu2data/NIS/EndingNis01.crowd). NULL, with `log`
// told why, when the file is missing or malformed.
Crowd* crowd_load(const char* path, void (*log)(const char*));
void crowd_free(Crowd* c);

// The scene chunk the file carries (the bundle's 0x80037020, header included)
// for nis_load_mem(); NULL when absent.
const void* crowd_scene_chunk(const Crowd* c, uint32_t* bytes);

int crowd_entity_count(const Crowd* c);
int crowd_car_count(const Crowd* c);

// The scene time `t` seconds the next crowd_draw() poses the crowd at.
void crowd_update(Crowd* c, float t);

// Poses (skins) and draws every entity in view of `vp`; one out of view is
// neither. Writes the vertex pools the GPU reads, so call it after
// C3D_FrameBegin (C3D_FRAME_SYNCDRAW: the last frame's draws are done). An
// entity already posed at this `t` is not skinned again. `scene` is the
// scene's placement, rows as axes and row 3 the origin (row-vector
// convention, as the entity matrices).
void crowd_draw(Crowd* c, const C3D_Mtx* vp, const float scene[4][3]);
// How many entities the last crowd_draw() drew.
int crowd_in_view(const Crowd* c);

// The ground the people stand on (#483): CBasicCharacterAnimEntity's ground
// follow (Init 0x8004FB0C, UpdateTimeStep 0x80050204) asks
// TopologyCoordinate::GetElevation under bone 1 and stands the entity's
// origin 0.05 m over it -- the first answer as it is, then halfway towards
// each new one. `fn` answers the ground under `p` (world space), false for
// none (the entity is left where it is). NULL turns it off.
void crowd_ground_source(bool (*fn)(const float p[3], float* outZ));

// CBasicCharacterAnimEntity::CreateFlares (Speed.elf 0x80050DCC, #497): for
// each flag girl carrying flares (FlagGirlFlareMap), her two flares at scene
// time `t` in world space -- `pos` the lit end, `dir` the flare's axis (not
// unit; FlagGirlFlareEffect::TriggerEffect takes both). `scene` as for
// crowd_draw(). Returns how many, at most `max` (two a girl).
int crowd_flares(Crowd* c, float t, const float scene[4][3], float pos[][3], float dir[][3],
	int max);

// CBasicCharacterAnimEntity::RenderEffects (Speed.elf 0x80050884, SPEED2.EXE
// 0x433220; #497): each person crowd_draw() last drew lies on a quad from
// BoneMap's first bone to its second -- the feet, L Foot 5 to R Foot 8 on the
// crowd's 24-bone skeletons, 5 to 9 on a 35-bone one -- 0.25 m past each
// along that line and 0.35 m to either side, each corner dropped onto the
// ground (crowd_ground_source()) and lifted 0.05 m. `quad` gets them in the
// ePoly's order, whose UVs are (0,0) (1,0) (1,1) (0,1). Returns how many, at
// most `max`; 0 when the scene's bundle has no CHARACTERSHADOW.
int crowd_shadow_quads(Crowd* c, float (*quad)[4][3], int max);
// The scene's CHARACTERSHADOW (black, its alpha the shadow), NULL with none.
const C3D_Tex* crowd_shadow_tex(const Crowd* c, const Tex3DS_SubTexture** sub);

// ZPM_Car`idx + 1`'s key count (15 a second), 0 with no track.
int crowd_car_keys(const Crowd* c, int idx);

// ZPM_Car`idx + 1`'s pose at scene time `t`, in scene space: `rot` rows are
// the car's axes, `pos` its origin. False when the scene has no such track.
bool crowd_car_pose(const Crowd* c, int idx, float t, float rot[3][3], float pos[3]);

#endif
