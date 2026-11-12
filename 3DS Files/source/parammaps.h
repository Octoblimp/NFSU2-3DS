#pragma once
#include <3ds.h>
#include <stdbool.h>

// Retail's ParameterMaps -- the per-area tables that make one part of the city
// look different from the next (TASKS.md #253).
//
// Where the data lives
// --------------------
// TRACKS/L4RA.BUN, the same retail resident track file world.c reads its
// visible-section tables from: one 0x8003B600 group (36908 bytes on the PC)
// holding one 0x8003B601 layer per table. A layer's sub-chunks:
//
//   0x0003B602  header: u32 name hash (bStringHash), float x0, y0, x1, y1
//               (the XY bounds), s32 node count, set count, row stride, field
//               count
//   0x0003B603  s32 per field: 0 float, 1 int
//   0x0003B604  s32 per field: its byte offset in a row
//   0x0003B605  the rows: set count x stride bytes
//   0x0003B607  the quadtree, 4 x u8 per node   (or)
//   0x0003B608  the quadtree, 4 x u16 per node
//
// The layers L4RA ships, by name where the hash is in Speed.elf: Night Normal
// Fog, Normal Fog, Night Screen Tint, Sunrise Screen Tint, Rain, Wind, Night
// Raining Fog, Horizon Fog Visibility, plus Volumes and three unnamed ones.
//
// The lookup (GetParameterSetIndexFromQuadData16, Speed.elf) is a plain
// region quadtree over the bounds: a point outside them is set 0, a node
// whose first child is 0 is a leaf naming its set in the second, and
// otherwise the cell is halved on both axes and children 0..3 are (low x,
// low y), (low x, high y), (high x, low y), (high x, high y).
//
// Set 0 is each layer's neutral row -- Night Screen Tint's is grey 128 at
// intensity 0 -- so a point off the map, and a port without the file, both
// read as no effect.

typedef struct ParamMaps ParamMaps;
typedef struct ParamLayer ParamLayer;

// Reads the group out of the BUN at `path`. NULL (and one log line) when the
// file or the group is missing; every caller treats that as "no per-area
// effects", never as an error.
ParamMaps* parammaps_open(const char* path, void (*log)(const char* msg));
void parammaps_close(ParamMaps* pm);

// A layer by its retail name ("Night Screen Tint"), or NULL.
const ParamLayer* parammaps_layer(const ParamMaps* pm, const char* name);

int parammaps_set_count(const ParamLayer* l);
int parammaps_field_count(const ParamLayer* l);
// The set index at world XY.
int parammaps_set_at(const ParamLayer* l, float x, float y);
// One field of one set, as a float whatever its stored type.
float parammaps_field(const ParamLayer* l, int set, int field);

// ParameterAccessorBlendByDistance: the values the game actually uses, eased
// from one set to the next as the car drives rather than switched at the cell
// edge. CaptureData (Speed.elf) blends the buffer toward the set under (x, y)
// by t = moved / blendDist, where `moved` is how far the point went since the
// last capture -- so a parked car holds whatever it has, and the change
// completes over about blendDist metres of driving at any frame rate. The
// first capture, and one after a jump of blendDist or more, copy the set
// outright. Int fields are rounded, as Blend::CaptureData does.
#define PARAM_ACCESSOR_FIELDS 8

typedef struct {
	const ParamLayer* layer;
	float values[PARAM_ACCESSOR_FIELDS];
	float lastX, lastY;
	bool  captured;
} ParamAccessor;

// A NULL layer is allowed: values stay at 0 and capture does nothing.
void paramaccessor_init(ParamAccessor* a, const ParamLayer* l);
void paramaccessor_capture(ParamAccessor* a, float x, float y, float blendDist);
