// A car's damage as retail keeps it (TASKS.md #324): CarDamage (Speed.elf
// 0x8024C3A4), its BodyDamage's 32 sectors round the car and the 15
// breakable items (BreakableCarDamageInfoTable, 0x80372120; SPEED2.EXE has
// the same table). What the port shows of it is the cracked windows: retail's
// body deformation (the BodyDamage vertex chunk 0x34030) ships in no PC car,
// and CarPartDamage's wobbling parts are switched off in both builds.
//
// Damage collects from every impulse a car's body takes while the race
// collects it (TheRaceParameters +0x38: every race but a drift, and free
// roam), and shows only once something turns it visible: the crash camera
// (SetDamageToFull) or a drag race's totalled car.
#pragma once

#include <3ds/types.h>
#include <stdbool.h>

#include "carsim.h"

#define CARDAMAGE_SECTORS 32
#define CARDAMAGE_ITEMS   15
// Items 8..13 are the glass, in UpdateCarReplacementTextures' order: front,
// rear, left front, left rear, right front, right rear.
#define CARDAMAGE_GLASS0  8
#define CARDAMAGE_WINDOWS 6

typedef struct {
	u8   sector[CARDAMAGE_SECTORS];     // BodyDamage +8: 0..128, 128 = whole
	s8   itemSector[CARDAMAGE_ITEMS];   // CarDamage +0xE8, -1 for no marker
	u32  state;                         // +0xE0: 2 bits an item
	u32  applied;                       // +0xE4: the state last shown
	bool visible;                       // +8
	u32  fresh;                         // items ApplyDamage moved to a level > 0 (#513)
	float lo[2], hi[2];                 // BodyDamage +0x4C..+0x58: the render box, x and y
} CarDamage;

// The position marker each item is found by on the car's models, by item.
extern const u32 cardamage_item_marker[CARDAMAGE_ITEMS];

// TheRaceParameters +0x38 (DAT_803FCA78): whether damage collects at all.
// GetVisibleDamageAmount reads 0 without it.
void cardamage_set_collecting(bool on);

// The CarDamage ctor plus CarRenderInfo's SetBBox and InitSectorPositions:
// `lo`/`hi` the render box (CarRenderInfo +0x10 / +0x20, x and y), and each
// item's marker translation in car space where `itemMask` has its bit.
void cardamage_init(CarDamage* d, const float lo[2], const float hi[2],
	const float (*itemXY)[2], u32 itemMask);
// Back to undamaged and hidden, the box and the items kept: a new car.
void cardamage_reset(CarDamage* d);

// CarCollisionBody::ApplyImpulse's AddDamage (0x801B0488) for car `s`: the
// impulse it takes (either sign), at world point `pos`.
void cardamage_impulse(CarDamage* d, const CarSim* s, const float pos[3], const float imp[3]);

void cardamage_full(CarDamage* d);                // SetDamageToFull (0x8024C988)
void cardamage_zero(CarDamage* d);                // SetDamageToZero (0x8024C9E4)
void cardamage_set_visible(CarDamage* d, bool on); // SetVisibleDamageEnabled (0x8024C4A4)
// CarDamage::ApplyDamage's per-item effect (#513): the items it moved to a
// new, non-zero level since the last take, a bit an item, cleared by the
// take. The caller fires each item's cardamage_item_effect() at
// cardamage_break_intensity[cardamage_item_level()] (Car::TriggerEffect).
// PlayGlass, the sound ApplyDamage also asks for at level 1, is an empty
// function in retail (EAXSound::PlayGlass, Speed.elf 0x8007D0E4).
u32 cardamage_take_breaks(CarDamage* d);
int cardamage_item_level(const CarDamage* d, int item);
// BreakableCarDamageInfoTable +0x1C: the item's CarEffect (0 for none).
int cardamage_item_effect(int item);
// BreakableCarDamageIntensities (0x8037233C), by level.
extern const float cardamage_break_intensity[4];

// The level (0..3) window `w` (0..5) shows: DamagedWindowHashes' index.
int cardamage_window(const CarDamage* d, int w);
