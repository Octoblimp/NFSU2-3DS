// The retail particle system ("Acid", after the AcidEmitter/AcidEmitterData
// chunk names), as far as free roam uses it: the floating mode icons, ground
// rings and smoke that mark every event, shop and bank, and the industrial
// smoke stacks (TASKS.md #174).
//
// The effects are data, not code. tools/afx_pack.py writes ACIDFX.nfsa next to
// the world: the AcidEmitterData templates byte for byte, the placed emitters
// out of STREAM<track>.BUN, the twelve trigger groups, and their textures.
// This file is the runtime half, transcribed from SPEED2.EXE:
//
//   TriggerEmitter   0x491e50  spawning, both cone modes, the per-emitter
//                              xorshift stream in retail's draw order
//   DoTimeStep       0x492e10  ageing, drag, gravity, atlas animation, the
//                              on/off timers and the condition gate
//   RenderEffects    0x48bdf0  colour and size curves, the screen-size clamp,
//                              upright billboards (list 0) and oriented quads
//                              (list 1)
//   SetEffectLocation 0x6054b0 / 0x602220  the trigger slots
//
// The props a car knocks loose carry their own emitters (afx_attach, below).
//
// What is not here: car-attached effects (tyre smoke, sparks -- nothing in
// free roam's data uses an owner), the per-player visibility masks that go
// with them, and the smaller pool at the lowest detail setting, which the
// GameCube build does not have either. The PC's low-detail spawn gate is
// afx_set_particles().
#ifndef AFX_H
#define AFX_H

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

// UpdateTriggerEffects' table (SPEED2.EXE 0x802300), in its order: one slot
// per group, which afx_pack.py writes in the same order.
//
// AFX_SLOT_DVD (XEE_DVD) is never lit, as in retail: the table matches it to
// trigger zone type 8, and GetTriggerZoneType (SPEED2.EXE 0x500DE0, GC
// 0x801289D0) never returns 8 -- shops give 9..14 (0x500340), events 0..5 and
// 7 (0x5002E0), showcases 6, world challenges 15. Only UpdateTriggerEffects
// reads the table (#242).
enum {
	AFX_SLOT_CIRCUIT = 0,
	AFX_SLOT_DRAG,
	AFX_SLOT_STREET,
	AFX_SLOT_DRIFT,
	AFX_SLOT_SPRINT,
	AFX_SLOT_URL,
	AFX_SLOT_SHOWCASE,
	AFX_SLOT_WORLD_CHALLENGE,
	AFX_SLOT_SUV,
	AFX_SLOT_DVD,
	AFX_SLOT_GAME_TIP,
	AFX_SLOT_BANK,
	AFX_SLOTS
};

// The camera the particles face. right/up/fwd are the view's world-space basis
// (unit, right-handed: right = fwd x up). projScale is screen pixels per unit
// of size/depth on the 240-line top screen, which is what the per-emitter
// maximum screen size is measured against.
typedef struct {
	float eye[3];
	float right[3];
	float up[3];
	float fwd[3];
	float projScale;
} AfxView;

typedef struct Afx Afx;

void afx_set_log(void (*sink)(const char* msg));

// ACIDFX.nfsa at `path`. NULL when it is absent or malformed -- a card packed
// before this build has none, and the world has to run without it -- which is
// said in the log.
Afx* afx_open(const char* path);
void afx_close(Afx* a);

// The one scenery condition that is on (0x5d7440 asks a list of eight; free
// roam only ever puts one name in it): FREE_ROAM, FREE_ROAM_STAGE0 or
// FREE_ROAM_STAGE0A, as bStringHash. 0 turns every gated emitter off; the
// emitters with no condition run regardless.
// `also` is a second name on the list (0 for none). PORT (TASKS.md #381): the
// stage 0 career puts FREE_ROAM there too, so the shops' circles show before
// the first garage visit; retail lists only the stage 0 name.
void afx_set_condition(Afx* a, u32 hash, u32 also);

// g_ParticleSystemEnable (#326). Off, an emitter spawns only if its template's
// +0x21 byte is set, as TriggerEmitter's check (SPEED2.EXE 0x491e50) has it.
// Particles already alive live out their time.
void afx_set_particles(bool on);

// SetEffectLocation: puts trigger slot `slot` at `pos` (the zone's entry point
// and Z; the caller resolves a zero Z to the ground first). A slot already
// within a millimetre of `pos` is left alone; one that moves is cleared --
// particles and all -- and its group copied afresh, as retail does.
void afx_set_trigger(Afx* a, int slot, const float pos[3]);
// 0x602220: the slot's emitters and every particle they own go at once.
void afx_clear_trigger(Afx* a, int slot);

// One DoTimeStep of `dt` seconds. `view` is the camera position: an emitter
// only spawns while it is within its own maximum distance of it.
void afx_update(Afx* a, float dt, const float view[3]);

// RenderEffects into the current target, after the opaque world. Leaves the
// GPU's shader, attribute and TEV state changed, which the caller's 2D pass
// (C2D_Prepare) resets; blend and depth it puts back itself, because
// C2D_Prepare does not.
void afx_draw(Afx* a, const C3D_Mtx* vp, const AfxView* view);

// The props' emitters (smokeable.c, TASKS.md #318): a Smokeable's copies of
// the emitter group its SmokeableInfo names -- the crash barrels' water, the
// greenhouse door's glass -- which afx_pack.py packs after the trigger
// groups. afx_attach starts each member at `m` (rows: x, y, z axes, then the
// position; m[15] 1), state 1, emitting for its on-period
// (Smokeable::Awaken 0x801D4CE4 and ActivateAnchorEmitters 0x801D4768 calling
// AddAutoEmitter 0x800C1208), and writes their handles to `out`. Returns how
// many started: 0 for a group the pack does not have or with every prop
// emitter busy.
#define AFX_PROP_EMITTERS 4
int afx_attach(Afx* a, u32 group, const float m[16], s16 out[AFX_PROP_EMITTERS]);
// ServiceEmitters (0x801D4418): where the emitter is now (`m`, NULL to leave
// it) and the velocity its particles inherit (`vel`, NULL to leave it).
void afx_attach_move(Afx* a, s16 h, const float m[16], const float vel[3]);
// The prop is gone: the emitter stops; its particles live out their lives.
void afx_detach(Afx* a, s16 h);

// A car's effects (TASKS.md #372, the Nitrous Purge spray; carfx.c). A pack
// with no placed emitters or groups -- GLOBAL/CARFX.nfsa -- holds the
// templates, by index.
int afx_tmpl_count(const Afx* a);
// The template's +0x5c: which of CarRenderInfo's position lists its copies
// fire at (28 the purge's hood nozzles, 29 its wheel nozzles).
u32 afx_tmpl_pos_type(const Afx* a, int tmpl);
// A copy of template `tmpl` in one of the attach slots, as afxCopyEmitter
// leaves it: never emitting on its own, only through afx_burst. -1 when no
// slot is free. afx_detach lets it go.
s16 afx_car_emitter(Afx* a, int tmpl);
// CarRenderInfo::TriggerEffect's step for one copy at one position: the copy
// moved to `m` (rows as afx_attach), inheriting `vel` (NULL to leave it), then
// TriggerEmitter(-intensity) -- one burst of `intensity` seconds' worth.
// `color` (16 floats, kept by the caller; NULL for the template's) is the
// colour cubic its particles are drawn with, as the purge parts' tints are.
void afx_burst(Afx* a, s16 h, const float m[16], const float vel[3], float intensity,
	const float* color, const float view[3]);

// For the status readout: live emitters (placed, trigger copies and props')
// and particles.
void afx_stats(const Afx* a, int* emitters, int* particles);

// Debug (#317): up to `max` of emitter `h`'s particles -- position, life
// fraction, colour (0..255) and size. Returns how many.
// Debug (#317): the quads the last afx_draw() put up; -1 when it ran with none.
int afx_last_quads(const Afx* a);
float afx_last_area(const Afx* a);
// The shipped cap (#521): none -- markers keep their PC size (the user).
#define AFX_SCREEN_CAP_3DS 0.0f
void afx_set_screen_cap(float px);
void afx_set_lean(bool on);   // #521: faint particles / zero-alpha texels skipped (default on)   // #521: 240-line px, 0 = the template's only   // #521: px^2 on the top screen
int afx_probe(const Afx* a, s16 h, int max, float (*pos)[3], float* tl, float (*col)[4],
	float* size);

#endif
