// retail's vAABBTree, as the per-section packs carry it: the light packs
// (lightdb.h) and the event-manager triggers (emevents.h). Nodes are 0x30
// bytes, little-endian as the PC files hold them:
//   +0x00 f32 centre[3]  +0x0c s16 parent  +0x0e s16 count (children; a leaf
//   has -items)  +0x10 f32 half[3]  +0x1c s16 index[10] (child nodes, or a
//   leaf's items)
// Node 0 is the root.
#pragma once

#include <3ds.h>

#define VAABB_NODE 0x30

// vAABBTree::QueryLeaf (Speed.elf 0x801a103c): the leaf holding `p`, or NULL.
// A point on a box's face is outside it (vAABB::Contains, 0x801a0d40).
const u8* vaabb_query_leaf(const u8* nodes, const float p[3]);

// A leaf's item count and its i-th item.
int vaabb_leaf_count(const u8* leaf);
int vaabb_leaf_item(const u8* leaf, int i);
