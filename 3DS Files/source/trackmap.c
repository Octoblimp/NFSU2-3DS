#include "trackmap.h"
#include "texpack.h"
#include <stdio.h>
#include <string.h>

// See trackmap.h for the SPEED2.EXE provenance of both format strings and of
// the widget/texture pairing this feeds.

void trackmap_init(TrackMap* m) {
	memset(m, 0, sizeof(*m));
	m->trackId = -1;
}

void trackmap_free(TrackMap* m) {
	if (m->valid)
		C3D_TexDelete(&m->tex);
	memset(m, 0, sizeof(*m));
	m->trackId = -1;
}

u32 trackmap_texture_hash(int trackId) {
	// Same bStringHash as trackdb_track_name_hash(): seed 0xFFFFFFFF,
	// h = c + h * 0x21. Applied to the name the .tpk stores, which for these
	// packs is already upper-case, so no ToUpper step is needed (the resource
	// path "TrackMap4011.tga" hashes to the same value through the
	// GetBaseName -> ToUpper -> bStringHash pipeline -- see trackmap.h).
	char key[32];
	snprintf(key, sizeof(key), "TRACKMAP%d", trackId);
	u32 h = 0xFFFFFFFFu;
	for (const unsigned char* p = (const unsigned char*)key; *p; p++)
		h = (u32)*p + h * 0x21u;
	return h;
}

void trackmap_rel_path(int trackId, char* out, size_t outSize) {
	snprintf(out, outSize, "TRACKS/TRACKMAP%d.BIN", trackId);
}

bool trackmap_load(TrackMap* m, const char* filePath, int trackId, int maxDim) {
	if (m->valid && m->trackId == trackId)
		return true;

	trackmap_free(m);

	// The .BIN is one 0xB3300000 container, i.e. exactly what texpack_load()
	// scans for. It is ~342KB on disk, well inside what the frontend already
	// holds, and it is released again as soon as its one texture is decoded.
	TexPack pack;
	if (!texpack_load(filePath, &pack))
		return false;

	u32 hash = trackmap_texture_hash(trackId);
	const TexPackEntry* e = texpack_find(&pack, hash);
	// These packs hold exactly one texture. If the name hash somehow misses
	// (an SKU whose pack names its texture differently), take the sole entry
	// rather than showing nothing -- the file was selected by track id, so
	// there is no other track's map it could be.
	if (!e && pack.count == 1)
		e = &pack.entries[0];
	if (!e) {
		texpack_free(&pack);
		return false;
	}

	// opaqueIfAlphaEmpty: these ship as DXT3 with an entirely empty alpha
	// block (see trackmap.h) -- without this the map uploads fully transparent
	// and the panel renders blank, which is what the first Azahar pass showed.
	bool ok = texpack_decode_entry(e, maxDim, true, &m->tex, &m->subtex);
	texpack_free(&pack);
	if (!ok)
		return false;

	m->image.tex = &m->tex;
	m->image.subtex = &m->subtex;
	m->trackId = trackId;
	m->valid = true;
	return true;
}
