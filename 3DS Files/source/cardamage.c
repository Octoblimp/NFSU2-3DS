// CarDamage, BodyDamage and the breakable items (TASKS.md #324). See
// cardamage.h. Decompiles: tools/re/wip296dmg/damage.c (Speed.elf) and
// damage_ps.c (SpeedPS.elf, where the GC image's are bad data).
#include "cardamage.h"

#include <math.h>
#include <string.h>

// BodyDamage::SetDamage (0x8024B744) keeps each sector as a byte, the amount
// x 128 (0x8033E3D4) clamped to 0..1; GetVisibleDamageAmount reads it back
// x 1/128 (0x8033E408).
#define DMG_SCALE    128.0f
// CarCollisionBody::AddDamage (0x801B0488): the impulse's size x
// ImpactDamageIntensityScale (0x8046E33C).
#define DMG_IMPACT   0.013f
// CarDamage::AddDamage (0x8024C5B0): nothing under 0.05 (0x8033E5A8); the hit
// sector takes it all, and the four either side of it half as much again
// each step round (0x8033E5AC).
#define DMG_MIN      0.05f
#define DMG_FALLOFF  0.5f
#define DMG_SPREAD   5
// AddDamageToSector (0x8024C748): a hit under 0.2 (0x8033E5C4) that would
// change what is broken is taken back.
#define DMG_BREAK    0.2f
// SetDamageToFull (0x8024C988): every sector by 1.0 (0x8033E5C8), as a hit of
// 9999 (0x8033E5CC).
#define DMG_FULL     1.0f
#define DMG_FULL_HIT 9999.0f
// CarCollisionBody::ApplyImpulse (0x801B0350): a body whose inverse mass
// (RigidBody +0x154) is over 0.5405405 (0x80331F44) has the impulse scaled
// by 0.5405405 / it first -- the collision took the car as 1.85 t.
#define DMG_INV_MASS 0.5405405f

// BreakableCarDamageInfoTable (Speed.elf 0x80372120, SPEED2.EXE 0x8036F8 --
// the same 15 rows, 0x24 bytes each): +4 the marker, +0xC..+0x14 the
// amounts at which the item reaches level 1, 2 and 3 (+8 is 0).
static const struct {
	u32 marker;
	float level[3];
} kItem[CARDAMAGE_ITEMS] = {
	{ 0xC4EC23C8u, { 0.25f, 0.50f, 0.75f } },  // Side Mirror Left
	{ 0x0BEA251Bu, { 0.25f, 0.50f, 0.75f } },  // Side Mirror Right
	{ 0xAA35C17Fu, { 0.25f, 0.50f, 0.75f } },  // License Plate Rear
	{ 0xC805F7E0u, { 0.25f, 0.50f, 0.75f } },  // License Plate Front
	{ 0x9DB90133u, { 0.25f, 0.35f, 0.50f } },  // Headlight Left
	{ 0xD09091C6u, { 0.25f, 0.35f, 0.50f } },  // Headlight Right
	{ 0x31A66786u, { 0.25f, 0.45f, 0.65f } },  // Brakelight Left
	{ 0xBF700A79u, { 0.25f, 0.45f, 0.65f } },  // Brakelight Right
	{ 0x6EC7E79Fu, { 0.15f, 0.30f, 0.45f } },  // Window Front
	{ 0x3F68F920u, { 0.05f, 0.10f, 0.15f } },  // Window Rear
	{ 0x3DF28F69u, { 0.05f, 0.10f, 0.15f } },  // Window Left Front
	{ 0xE898CFAAu, { 0.05f, 0.10f, 0.15f } },  // Window Left Rear
	{ 0x64B0BB1Cu, { 0.05f, 0.10f, 0.15f } },  // Window Right Front
	{ 0x2F96D0FDu, { 0.05f, 0.10f, 0.15f } },  // Window Right Rear
	{ 0x68100815u, { 0.70f, 0.85f, 0.95f } },  // Radiator
};

const u32 cardamage_item_marker[CARDAMAGE_ITEMS] = {
	0xC4EC23C8u, 0x0BEA251Bu, 0xAA35C17Fu, 0xC805F7E0u, 0x9DB90133u,
	0xD09091C6u, 0x31A66786u, 0xBF700A79u, 0x6EC7E79Fu, 0x3F68F920u,
	0x3DF28F69u, 0xE898CFAAu, 0x64B0BB1Cu, 0x2F96D0FDu, 0x68100815u,
};

static bool s_collecting = true;

void cardamage_set_collecting(bool on) {
	s_collecting = on;
}

// BodyDamage::GetSectorNumber (0x8024B81C): the point's place in the box on
// an 8 x 8 grid, then which of the box's four sides it is nearest, numbered
// round the car: 16 - x along one side, 8 - y, y + 16, x + 24.
static int sectorOf(const CarDamage* d, const float p[2]) {
	const float wx = d->hi[0] - d->lo[0], wy = d->hi[1] - d->lo[1];
	const float fx = wx > 1e-6f ? (p[0] - d->lo[0]) / wx : 0.5f;
	const float fy = wy > 1e-6f ? (p[1] - d->lo[1]) / wy : 0.5f;
	int ix = (int)(fx * 8.0f), iy = (int)(fy * 8.0f);
	if (ix < 0) ix = 0;
	if (ix > 7) ix = 7;
	if (iy < 0) iy = 0;
	if (iy > 7) iy = 7;
	if (fx <= fy)
		return 1.0f - fy < fx ? ix + 24 : iy + 16;
	return 1.0f - fy < fx ? 8 - iy : 16 - ix;
}

// GetVisibleDamageAmount (0x8024B8F8): 0 while nothing collects.
static float visibleAmount(const CarDamage* d, int s) {
	return s_collecting ? (float)d->sector[s] * (1.0f / DMG_SCALE) : 0.0f;
}

// RecalculateBreakableDamage (0x8024C6A4): each item with a sector, the
// highest level its sector's amount has reached.
static u32 recalc(const CarDamage* d) {
	u32 st = 0;
	for (int i = 0; i < CARDAMAGE_ITEMS; i++) {
		if (d->itemSector[i] < 0)
			continue;
		const float a = visibleAmount(d, d->itemSector[i]);
		int lv = 3;
		while (lv > 0 && a < kItem[i].level[lv - 1])
			lv--;
		st |= (u32)lv << (i * 2);
	}
	return st;
}

static void setSector(CarDamage* d, int s, float v) {
	if (v > 1.0f) v = 1.0f;
	if (v < 0.0f) v = 0.0f;
	d->sector[s] = (u8)(int)(v * DMG_SCALE);
}

// BreakableCarDamageInfoTable (0x80372120) +0x1C, by item: the CarEffect
// each one triggers -- the CARFX_BREAK_* effects 15..29 (no 23, the centre
// brakelight, which no item has) and the radiator's CARFX_DAM_RADIATOR (6).
static const u8 kItemEffect[CARDAMAGE_ITEMS] = {
	15, 16, 17, 18, 19, 20, 21, 22, 24, 25, 26, 27, 28, 29, 6,
};
const float cardamage_break_intensity[4] = { 0.0f, 0.1f, 0.4f, 1.0f };

// CarDamage::ApplyDamage (0x8024C850), while the damage shows: the items
// moved to their new levels, each one moved to a level above 0 marked for
// its TriggerEffect (cardamage_take_breaks), and UpdateCarReplacementTextures.
// PORT: its PlayGlass is empty in retail, and BodyDamage::ApplyDamage has no
// vertex data in any PC car.
static void apply(CarDamage* d) {
	if (!d->visible)
		return;
	d->state = recalc(d);
	for (int i = 0; i < CARDAMAGE_ITEMS; i++) {
		const u32 lv = (d->state >> (i * 2)) & 3u;
		if (lv != ((d->applied >> (i * 2)) & 3u) && lv != 0)
			d->fresh |= 1u << i;
	}
	d->applied = d->state;
}

u32 cardamage_take_breaks(CarDamage* d) {
	if (!d)
		return 0;
	const u32 f = d->fresh;
	d->fresh = 0;
	return f;
}

int cardamage_item_level(const CarDamage* d, int item) {
	if (!d || item < 0 || item >= CARDAMAGE_ITEMS)
		return 0;
	return (int)((d->applied >> (item * 2)) & 3u);
}

int cardamage_item_effect(int item) {
	return item >= 0 && item < CARDAMAGE_ITEMS ? kItemEffect[item] : 0;
}

// AddDamageToSector (0x8024C748): `amount` onto sector `s`, `hit` the size of
// the hit it came from.
static void addSector(CarDamage* d, float amount, float hit, int s) {
	const float cur = (float)d->sector[s] * (1.0f / DMG_SCALE);
	float v = cur + amount;
	if (v < 0.0f) v = 0.0f;
	if (v > 1.0f) v = 1.0f;
	if (v == cur)
		return;
	setSector(d, s, v);
	const u32 st = recalc(d);
	if (st != d->state) {
		if (hit >= DMG_BREAK)
			d->state = st;
		else
			setSector(d, s, cur);
	}
}

void cardamage_init(CarDamage* d, const float lo[2], const float hi[2],
		const float (*itemXY)[2], u32 itemMask) {
	memset(d, 0, sizeof(*d));
	d->lo[0] = lo[0];
	d->lo[1] = lo[1];
	d->hi[0] = hi[0];
	d->hi[1] = hi[1];
	for (int i = 0; i < CARDAMAGE_ITEMS; i++)
		d->itemSector[i] = (itemMask >> i) & 1u ? (s8)sectorOf(d, itemXY[i]) : (s8)-1;
}

void cardamage_reset(CarDamage* d) {
	memset(d->sector, 0, sizeof(d->sector));
	d->state = d->applied = 0;
	d->fresh = 0;
	d->visible = false;
}

void cardamage_impulse(CarDamage* d, const CarSim* s, const float pos[3], const float imp[3]) {
	if (!d || !s || !s_collecting)
		return;
	float j = sqrtf(imp[0] * imp[0] + imp[1] * imp[1] + imp[2] * imp[2]);
	if (s->invMass > DMG_INV_MASS)
		j *= DMG_INV_MASS / s->invMass;
	const float amount = j * DMG_IMPACT;
	if (amount < DMG_MIN)
		return;
	// The contact from the body's centre, turned into the car's frame.
	const float r[3] = { pos[0] - s->rbPos[0], pos[1] - s->rbPos[1], pos[2] - s->rbPos[2] };
	const float p[2] = {
		s->rot[0][0] * r[0] + s->rot[0][1] * r[1] + s->rot[0][2] * r[2],
		s->rot[1][0] * r[0] + s->rot[1][1] * r[1] + s->rot[1][2] * r[2],
	};
	const int sec = sectorOf(d, p);
	addSector(d, amount, amount, sec);
	float a = amount;
	for (int i = 1; i < DMG_SPREAD; i++) {
		a *= DMG_FALLOFF;
		addSector(d, a, amount, (sec + i) & (CARDAMAGE_SECTORS - 1));
		addSector(d, a, amount, (sec - i) & (CARDAMAGE_SECTORS - 1));
	}
	apply(d);
}

void cardamage_full(CarDamage* d) {
	for (int s = 0; s < CARDAMAGE_SECTORS; s++)
		addSector(d, DMG_FULL, DMG_FULL_HIT, s);
	apply(d);
}

void cardamage_zero(CarDamage* d) {
	// BodyDamage::RepairAllDamage (0x8024B7AC).
	memset(d->sector, 0, sizeof(d->sector));
	apply(d);
}

void cardamage_set_visible(CarDamage* d, bool on) {
	if (d->visible == on)
		return;
	d->visible = on;
	if (on)
		apply(d);
}

int cardamage_window(const CarDamage* d, int w) {
	if (!d || w < 0 || w >= CARDAMAGE_WINDOWS)
		return 0;
	return (int)((d->applied >> ((CARDAMAGE_GLASS0 + w) * 2)) & 3u);
}
