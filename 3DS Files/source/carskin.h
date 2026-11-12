// The car's skin: retail's one composited body texture (TASKS.md #296).
//
// NFSU2 has no paint shader. Whenever a car's look changes, CompositeSkin
// (SPEED2.EXE 0x61D790) gathers its layers and CompositeSkin32 (0x611F10)
// blends them on the CPU into one of the DUMMY_SKINn textures, which
// SwitchSkin (0x636840) hands every body slot: <CAR>_SKIN1, GLOBAL_SKIN1,
// SPOILER_SKIN, ROOF_SKIN and the two unnamed body slots. The base colour,
// the vinyls, the door lines, and the colours of the spoiler, roof scoop,
// mirrors, brakes, exhaust tips, engine and audio (seven 4x4 swatches the
// parts' UVs point at) are all texels of that one texture.
//
// This is that composite, over what tools/vinyl_pack.py packs as
// CARS/<CAR>.nfsv; carkit.c points the skin slots of every part at the
// <CAR>_SKIN1 placeholder tools/kit_pack.py leaves in the car's kit, and
// model3d_set_skin() uploads the result into it.
#pragma once

#include <3ds.h>
#include "paint.h"

// CompositeSkin's region list, in swatch order: the CarSlotIds PAINT_ENGINE
// 0x45, PAINT_SPOILER 0x46, PAINT_BRAKE 0x47, PAINT_EXHAUST 0x48, PAINT_AUDIO
// 0x49, PAINT_ROOF 0x4C and PAINT_MIRROR 0x4D.
#define CARSKIN_REGIONS 7
#define CARSKIN_VINYLS  4
#define CARSKIN_REMAPS  4

typedef struct {
	// The save's BASE_PAINT part (CarSlotId 0x40), 0 for none.
	u32 basePaint;
	// Each region's paint part, 0 for one that takes the base colour. The
	// audio gate (its region keeps the base under anything short of a
	// level-3 TRUNK_AUDIO install) is already applied.
	u32 regionPart[CARSKIN_REGIONS];
	// VINYL_LAYER0..3 (CarSlotId 0x41..0x44), 0 when empty.
	u32 vinyl[CARSKIN_VINYLS];
	// Each layer's remap colours, CarSlotId 0x4E + layer * 4 + k, 0 when empty.
	u32 vinylColour[CARSKIN_VINYLS][CARSKIN_REMAPS];
	// The doorline set FUN_00612B70 picks: "", "_KIT" or "_WIDEBODY".
	char doorline[12];
	// What carskin_wheels() reads: the FRONT_WHEEL part (CarSlotId 0x1E) and
	// the PAINT_RIM / PAINT_SPINNER parts (0x4A / 0x4B), 0 when empty.
	u32 frontWheel;
	u32 rimPaint;
	u32 spinnerPaint;
} CarSkinDesc;

// The skin `d` describes for `car` (its CARS/ folder name), as 0xAARRGGBB
// texels, row 0 the top, alpha the paint's GLOSS: malloc'd, *w by *h, for the
// caller to free. `dataRoot` is the game data directory as a full path;
// `paint` is PAINT.nfsp (NULL leaves every colour at the fallback). A card
// without the car's .nfsv still gets a skin -- the base colour alone, with
// no swatches -- and `log` (may be NULL) says so. NULL only when out of memory.
u32* carskin_compose(const char* dataRoot, const char* car, const CarSkinDesc* d,
	const PaintTable* paint, int* w, int* h, void (*log)(const char* msg));

// The rim and the spinner. CompositeSkin ends with CompositeRim and
// CompositeSpinner (SPEED2.EXE 0x6343A0 / 0x61E040), which paint the front
// wheel part's art into DUMMY_WHEELn / DUMMY_SPINNERn; FUN_00631E20's swap
// then hands those to every mesh asking for <TEXTURE_NAME>_WHEEL or the
// SPINNER_TEXTURE. Each result here carries the hash it stands in for, for
// model3d_set_texture().
#define CARSKIN_WHEEL_TEXS 2
typedef struct {
	u32 hash;    // the rim or spinner texture it replaces
	u32* argb;   // w * h 0xAARRGGBB, row 0 the top, malloc'd
	int w, h;
} CarSkinTex;

// Composites `d`'s rim and spinner over CARS/WHEELS.nfsr (tools/rim_pack.py)
// into out[] and returns how many it filled, each for the caller to free.
// None without a BASE_PAINT (retail's CompositeSkin returns before them), a
// front wheel part or the file; `log` (may be NULL) says which.
int carskin_wheels(const char* dataRoot, const CarSkinDesc* d, const PaintTable* paint,
	CarSkinTex out[CARSKIN_WHEEL_TEXS], void (*log)(const char* msg));

// TASKS.md #402: while held, carskin_compose() keeps the car's .nfsv open and
// every texture it inflated (up to 4 MB) for the next composite -- a shop's
// vinyl previews. hold(false) frees them.
void carskin_cache_hold(bool hold);
