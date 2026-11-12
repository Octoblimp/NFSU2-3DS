// The frontend car's own particle effects (TASKS.md #372): the Nitrous Purge
// spray GarageMainScreen::TriggerCarEffect puts on the shop car while Y is held
// on the Nitrous Purge list.
//
// The data is GLOBAL/CARFX.nfsa (tools/carfx_pack.py): an afx.c pack of the
// XES_NOSBO_* emitter templates and SFX_SMOKE_B_CS, with the purge parts'
// table appended -- which emitter groups each part names, which nozzles it
// fits, and its colour cubics already tinted. This is the runtime half:
//
//   UpdateCarParts      Speed.elf 0x802334D0  the purge half: effect lists 13
//                                             and 14 from the part's groups,
//                                             the nozzle positions
//   TriggerEffect       0x80238A78            every copy bursts once at each
//                                             position of its kind
//
// The world's nitrous flame (#317) comes from the same pack: effect 10,
// CARFX_NITRO (XES_NOS_FlameA), at CarRenderInfo's exhaust list:
//
//   TriggerCarTerrainEffects  0x80221D38  every tick the bottle burns and the
//                                         car is not blowing off: effect 10 at
//                                         1.0; a gear-change backfire: effect
//                                         10 at level / 3.5
//   Car::TriggerEffect        0x80230FA8  the car's matrix and position, its
//                                         velocity for the particles'
//   CarRenderInfo ctor        0x802314C0  list 10: the exhaust markers'
//                                         translations, no flips
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include "afx.h"
#include "carkit.h"

void carfx_set_log(void (*sink)(const char* msg));

// CARFX.nfsa at `path`. False when it is missing or malformed -- a card
// packed before #372 -- and the purge then only sounds; the log says which.
bool carfx_open(const char* path);
void carfx_close(void);
bool carfx_is_open(void);

// UpdateCarParts' purge half for the frontend's car: `part` is the car's
// slot 0xA2 part (0 for none), `kit` its assembly (the hood markers), and
// `wheel` the front wheels' CarPhysicsInfo positions and rolling radii,
// FL then FR, as { x, y, z, radius } in car space (NULL when the car's record
// was not found: no wheel nozzles). A part already set is left alone. The
// request outlives carfx_close(): carfx_open() builds it again (#478).
void carfx_set_purge(u32 part, const CarKitInfo* kit, const float wheel[2][4]);

// FETriggerCarEffect(13): one burst from every nozzle, the car at `rot`
// (rows: forward, left, up) and `origin`, as carneon_draw() takes it. `eye`
// is the camera, which the emitters' maximum distance is measured from.
void carfx_trigger_purge(const float rot[3][3], const float origin[3], const float eye[3]);

// DoTimeStep and RenderEffects for the car's particles; see afx.h.
void carfx_update(float dt, const float eye[3]);
void carfx_draw(const C3D_Mtx* vp, const AfxView* view);

// CarRenderInfo's exhaust list for the player's car: kit->exhaust (none for
// a NULL kit). Kept across carfx_close(), as the world opens the pack again.
void carfx_set_exhaust(const CarKitInfo* kit);

// Car::TriggerEffect(CARFX_NITRO, intensity) on the player's car: one burst
// of every CARFX_NITRO copy at each exhaust tip. `rot` rows forward, left, up
// and `origin` as carfx_trigger_purge(); `vel` the car's world velocity, which
// the flame inherits. Its particles need carfx_update()/carfx_draw().
void carfx_trigger_nitro(float intensity, const float rot[3][3], const float origin[3],
	const float vel[3], const float eye[3]);
// The player's car's velocity this frame, every frame: DoTimeStep carries the
// flame's particles along by the holder car's current velocity (afx.c).
// The flag girl's flare (#497, FlagGirlFlareEffect::TriggerEffect): one
// burst of XECH_FLAG_GIRL_FLARE's glow, smoke and sparks at `pos` (world),
// thrown along `dir` (the flare's axis, any length) at 2 m/s.
void carfx_trigger_flare(const float pos[3], const float dir[3], const float eye[3]);
void carfx_car_velocity(const float vel[3]);
// Exhaust tips known, for the log.
int carfx_exhausts(void);

// The breakable items' glass (#513). CarRenderInfo's glass position lists for
// the player's car, kit->glassPos (none for a NULL kit); kept across
// carfx_close() as the exhaust tips are. Returns the positions known.
int carfx_set_glass(const CarKitInfo* kit);
// Car::TriggerEffect(effect, intensity) (Speed.elf 0x80230FA8) for one of
// CarDamage::ApplyDamage's items on the player's car: `effect` the item's
// CarEffect (CarEffectParameters index, 15..29 the CARFX_BREAK_* ones), each
// copy of its group bursting once at every position of its list. Effects
// whose group the PC data has no emitters for (the mirrors, the plates, the
// centre brakelight) do nothing, as in retail. Arguments as
// carfx_trigger_nitro().
void carfx_trigger_break(int effect, float intensity, const float rot[3][3],
	const float origin[3], const float vel[3], const float eye[3]);
// Debug (#317): logs a few flame particles in the car's frame.
void carfx_probe_nitro(const float rot[3][3], const float origin[3]);

// Particles alive, for the log.
int carfx_particles(void);
// The purge as it stands, for the log (#478): the part its lists are built
// for (0 for none or shut), its emitter copies, its nozzles, and the quads the
// last carfx_draw() put up (-2 shut, -1 nothing drawn).
void carfx_purge_state(u32* part, int* copies, int* nozzles, int* quads);
