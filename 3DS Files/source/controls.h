#pragma once
// PORT (TASKS.md #447, the user's ask on top of retail): the remappable
// controls. Retail's console Controls screen offers four fixed schemes
// (options.c's kControlActions, Control Type 1-4); this port replaces it with
// one default -- retail's shipped scheme 0, the layout the port has always
// driven on -- that each action can be moved off, saved per profile.
//
// The model is retail's own: fourteen controls (the slots of
// UI_OptionsController's picture), each performing one action. Slot 2 is the
// Circle Pad, which steers and is not a button, so Steer stays there; the
// other thirteen slots are the New 3DS's thirteen buttons that are not START
// (pause stays on START, as on every retail pad), and scheme 0 puts the
// thirteen other actions on them one each. No button does two things, but
// an action may have none (#527, the user: "it doesn't auto-swap, it just
// removes the conflicting control instead"): moving an action onto a button
// takes that button off the action that had it, which is left "Not Used";
// and the page's Delete All Controls takes every action off its button.
//
// The bindings live beside the profile's save as <save>.controls, a text file
// of "Action=Button" lines (display.h's .display is the same arrangement),
// which keeps the save itself byte for byte the PC's. A profile without the
// file, or with one that does not name every action once, gets the default.

#include <3ds.h>
#include <stdbool.h>

#define CONTROLS_SLOTS   14 // options.h OPTIONS_CONTROL_BUTTONS
#define CONTROLS_ACTIONS 13 // every action but Steer
#define CONTROLS_STICK_SLOT 2

// The KEY_ bits of slot `slot` (0 for the Circle Pad), and its name ("A",
// "D-Pad Up", ...).
u32 controls_slot_keys(int slot);
const char* controls_slot_name(int slot);

// The English.bin key (OPT_ACT_*) of rebindable action `i`, 0..12, in the
// order the Controls page lists them.
u32 controls_action_hash(int i);

// The action slot `slot` performs (OPT_ACT_STEER on the stick), 0 out of range.
u32 controls_action_at(int slot);

// The slot rebindable action `i` is on, -1 when it has none.
int controls_slot_of(int i);

// Moves action `i` to the next (right) or previous button, or to none (one
// stop past SELECT). A button another action had is taken off it (#527) --
// but while the same row keeps stepping, the action it took a button from
// gets that button back as the row moves on, so only the button the row
// stops on costs anybody theirs. controls_step_commit() ends a row's run
// (the cursor left it).
void controls_step(int i, bool right);
void controls_step_commit(void);

// Delete All Controls (#527): every action off its button.
void controls_clear(void);

// Retail's scheme 0.
void controls_default(void);
bool controls_is_default(void);

// <savePath>.controls; a missing or invalid file leaves the default. An
// action written "None" (or not written) has no button.
bool controls_load(const char* savePath);
bool controls_save(const char* savePath);

// "Accelerate=A Brake=B ..." for the log.
void controls_describe(char* out, int cap);

// The menus. The binding is the driving's only (the user, 2026-11-09: the
// Controls page "only changes controls for driving etc. and nothing else");
// every menu, dialog and prompt reads the real buttons, A confirming and B
// backing out whatever the binding says. These were #447's button swap
// (2026-11-09, "Menus follow the remap") and are the identity now, kept so the
// call sites read the same: controls_menu_keys the pad as the menus read it,
// controls_menu_phys the real buttons behind menu bits.
u32 controls_menu_keys(u32 phys);
u32 controls_menu_phys(u32 logical);
void controls_menu_commit(void);
