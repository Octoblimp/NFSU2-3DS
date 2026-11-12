// See vaabb.h.
#include "vaabb.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static u16 rd16(const u8* p) {
	return (u16)(p[0] | (p[1] << 8));
}

static float rdf(const u8* p) {
	const u32 v = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

// vAABB::Contains (0x801a0d40): strictly inside on every axis.
static bool contains(const u8* n, const float p[3]) {
	for (int a = 0; a < 3; a++)
		if (!(fabsf(p[a] - rdf(n + a * 4)) < rdf(n + 0x10 + a * 4)))
			return false;
	return true;
}

// vAABBTree::QueryLeafHelper (0x801a0f64): the first child holding `p`, in
// order, that is a leaf or has a leaf under it holding `p`.
static const u8* leafHelper(const u8* nodes, const u8* n, const float p[3]) {
	const s16 cnt = (s16)rd16(n + 0x0e);
	for (int i = 0; i < cnt; i++) {
		const u8* c = nodes + (size_t)rd16(n + 0x1c + i * 2) * VAABB_NODE;
		if (!contains(c, p))
			continue;
		if ((s16)rd16(c + 0x0e) <= 0)
			return c;
		const u8* r = leafHelper(nodes, c, p);
		if (r)
			return r;
	}
	return NULL;
}

const u8* vaabb_query_leaf(const u8* nodes, const float p[3]) {
	if (!contains(nodes, p))
		return NULL;
	if ((s16)rd16(nodes + 0x0e) <= 0)
		return nodes;
	return leafHelper(nodes, nodes, p);
}

int vaabb_leaf_count(const u8* leaf) {
	return -(s16)rd16(leaf + 0x0e);
}

int vaabb_leaf_item(const u8* leaf, int i) {
	return rd16(leaf + 0x1c + i * 2);
}
