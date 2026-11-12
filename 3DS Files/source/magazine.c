#include "magazine.h"
#include "jdlz.h"
#include "texpack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGDB_CHUNK      0x00030231u
#define MAGDB_PAYLOAD    0x138Cu
#define MAGDB_CAR_OFF    0x00Cu
#define MAGDB_CAR_SIZE   44
#define MAGDB_RENDER_OFF 0xB0Cu
#define MAGDB_RENDER_SIZE 12
#define MAGDB_MAG_OFF    0xC8Cu
#define MAGDB_MAG_SIZE   56

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static float rdf(const u8* p) {
	const u32 v = rd32(p);
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

bool magdb_load(MagDb* db, const char* path) {
	memset(db, 0, sizeof(*db));
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	u8 hdr[8];
	static u8 buf[MAGDB_PAYLOAD];
	const bool ok = fread(hdr, 1, 8, f) == 8 && rd32(hdr) == MAGDB_CHUNK &&
		rd32(hdr + 4) >= MAGDB_PAYLOAD && fread(buf, 1, MAGDB_PAYLOAD, f) == MAGDB_PAYLOAD;
	fclose(f);
	if (!ok)
		return false;
	const u32 nCar = rd32(buf), nRender = rd32(buf + 4), nMag = rd32(buf + 8);
	if (nCar > MAGDB_CARS || nRender > MAGDB_RENDERS || nMag > MAGDB_MAGS)
		return false;
	for (u32 i = 0; i < nCar; i++) {
		const u8* p = buf + MAGDB_CAR_OFF + i * MAGDB_CAR_SIZE;
		MagCarSetup* c = &db->car[i];
		c->hash = rd32(p);
		c->yaw = rdf(p + 8);
		c->elev = rdf(p + 12);
		c->dist = rdf(p + 16);
		c->bias[0] = rdf(p + 20);
		c->bias[1] = rdf(p + 24);
		c->bias[2] = rdf(p + 28);
		c->fov = rdf(p + 32);
		c->p8 = rdf(p + 36);
		c->p9 = rdf(p + 40);
	}
	for (u32 i = 0; i < nRender; i++) {
		const u8* p = buf + MAGDB_RENDER_OFF + i * MAGDB_RENDER_SIZE;
		db->render[i].hash = rd32(p);
		db->render[i].w[0] = rd32(p + 4);
		db->render[i].w[1] = rd32(p + 8);
	}
	for (u32 i = 0; i < nMag; i++) {
		const u8* p = buf + MAGDB_MAG_OFF + i * MAGDB_MAG_SIZE;
		MagSetup* m = &db->mag[i];
		m->hash = rd32(p);
		m->nMast = rd32(p + 8);
		m->nExtra = rd32(p + 12);
		m->nCar = rd32(p + 16);
		m->nRender = rd32(p + 20);
		if (m->nMast > 2 || m->nExtra > 1 || m->nCar > 3 || m->nRender > 2) {
			memset(db, 0, sizeof(*db));
			return false;
		}
		for (int k = 0; k < 2; k++)
			m->mast[k] = rd32(p + 24 + 4 * k);
		m->extra[0] = rd32(p + 32);
		for (int k = 0; k < 3; k++)
			m->car[k] = rd32(p + 36 + 4 * k);
		for (int k = 0; k < 2; k++)
			m->render[k] = rd32(p + 48 + 4 * k);
	}
	db->nCar = (int)nCar;
	db->nRender = (int)nRender;
	db->nMag = (int)nMag;
	return true;
}

const MagSetup* magdb_find_magazine(const MagDb* db, u32 hash) {
	for (int i = 0; i < db->nMag; i++)
		if (db->mag[i].hash == hash)
			return &db->mag[i];
	return NULL;
}

const MagCarSetup* magdb_find_car(const MagDb* db, u32 hash) {
	for (int i = 0; i < db->nCar; i++)
		if (db->car[i].hash == hash)
			return &db->car[i];
	return NULL;
}

void magdb_generate_selection(const MagSetup* m, MagSelection* sel, u32 (*rnd)(u32 n)) {
	int i = m->nMast < 2 ? 0 : (int)rnd(m->nMast);
	sel->mast = m->mast[i];
	i = m->nExtra < 2 ? 0 : (int)rnd(m->nExtra);
	// The extra is read through the magazine's own slots, so with none it is
	// the zero the file stores there.
	sel->extra = m->extra[0];
	i = m->nCar == 0 ? 0 : (int)rnd(m->nCar);
	sel->carIdx = i;
	sel->car = m->car[i];
	i = m->nRender == 0 ? 0 : (int)rnd(m->nRender);
	sel->render = m->render[i];
}

void magdb_cycle_car_setups(const MagSetup* m, MagSelection* sel) {
	const int last = (int)m->nCar - 1;
	if (sel->carIdx < last)
		sel->carIdx++;
	else if (sel->carIdx == last)
		sel->carIdx = 0;
	if (sel->carIdx >= 0 && sel->carIdx < 3)
		sel->car = m->car[sel->carIdx];
}

#define FEMAG_CHUNK       0x00030230u
#define FEMAG_PAYLOAD     0x1E0Cu
#define FEMAG_CAR_OFF     0x00Cu
#define FEMAG_CAR_SIZE    56
#define FEMAG_RENDER_OFF  0xE0Cu
#define FEMAG_RENDER_SIZE 24
#define FEMAG_MAG_OFF     0x110Cu
#define FEMAG_MAG_SIZE    0x68

bool femag_load(FeMagDb* db, const char* path) {
	memset(db, 0, sizeof(*db));
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	u8 hdr[8];
	u8* buf = (u8*)malloc(FEMAG_PAYLOAD);
	const bool ok = buf && fread(hdr, 1, 8, f) == 8 && rd32(hdr) == FEMAG_CHUNK &&
		rd32(hdr + 4) >= FEMAG_PAYLOAD && fread(buf, 1, FEMAG_PAYLOAD, f) == FEMAG_PAYLOAD;
	fclose(f);
	const u32 nCar = ok ? rd32(buf) : 0, nRender = ok ? rd32(buf + 4) : 0,
		nMag = ok ? rd32(buf + 8) : 0;
	if (!ok || nCar > FEMAG_CARS || nRender > FEMAG_RENDERS || nMag > FEMAG_MAGS) {
		free(buf);
		return false;
	}
	for (u32 i = 0; i < nCar; i++) {
		const u8* p = buf + FEMAG_CAR_OFF + i * FEMAG_CAR_SIZE;
		FeCarSetup* c = &db->car[i];
		c->hash = rd32(p);
		c->flags = rd32(p + 4);
		c->zRot = rdf(p + 8);
		c->tireAngle = rdf(p + 12);
		c->fov = rdf(p + 16);
		for (int k = 0; k < 3; k++) {
			c->target[k] = rdf(p + 20 + 4 * k);
			c->eye[k] = rdf(p + 32 + 4 * k);
		}
		c->dof[0] = rdf(p + 44);
		c->dof[1] = rdf(p + 48);
		c->roll = rdf(p + 52);
	}
	for (u32 i = 0; i < nRender; i++) {
		const u8* p = buf + FEMAG_RENDER_OFF + i * FEMAG_RENDER_SIZE;
		db->render[i].hash = rd32(p);
		db->render[i].shaper = rd32(p + 4);
		memcpy(db->render[i].envmap, p + 8, 16);
		db->render[i].envmap[16] = '\0';
	}
	bool fits = true;
	for (u32 i = 0; i < nMag; i++) {
		const u8* p = buf + FEMAG_MAG_OFF + i * FEMAG_MAG_SIZE;
		FeMagSetup* m = &db->mag[i];
		m->hash = rd32(p);
		m->nBack = rd32(p + 0x08);
		m->nMast = rd32(p + 0x0C);
		m->nChick = rd32(p + 0x10);
		m->nExtra = rd32(p + 0x14);
		m->nCar = rd32(p + 0x18);
		m->nRender = rd32(p + 0x1C);
		if (m->nBack > 3 || m->nMast > 2 || m->nChick > 5 || m->nExtra > 3 ||
				m->nCar > 3 || m->nRender > 2) {
			fits = false;
			break;
		}
		for (int k = 0; k < 3; k++) m->back[k] = rd32(p + 0x20 + 4 * k);
		for (int k = 0; k < 2; k++) m->mast[k] = rd32(p + 0x2C + 4 * k);
		for (int k = 0; k < 5; k++) m->chick[k] = rd32(p + 0x34 + 4 * k);
		for (int k = 0; k < 3; k++) m->extra[k] = rd32(p + 0x48 + 4 * k);
		for (int k = 0; k < 3; k++) m->car[k] = rd32(p + 0x54 + 4 * k);
		for (int k = 0; k < 2; k++) m->render[k] = rd32(p + 0x60 + 4 * k);
	}
	free(buf);
	if (!fits) {
		memset(db, 0, sizeof(*db));
		return false;
	}
	db->nCar = (int)nCar;
	db->nRender = (int)nRender;
	db->nMag = (int)nMag;
	return true;
}

const FeMagSetup* femag_find_magazine(const FeMagDb* db, u32 hash) {
	for (int i = 0; i < db->nMag; i++)
		if (db->mag[i].hash == hash)
			return &db->mag[i];
	return NULL;
}

const FeCarSetup* femag_find_car(const FeMagDb* db, u32 hash) {
	for (int i = 0; i < db->nCar; i++)
		if (db->car[i].hash == hash)
			return &db->car[i];
	return NULL;
}

const FeRenderSetup* femag_find_render(const FeMagDb* db, u32 hash) {
	for (int i = 0; i < db->nRender; i++)
		if (db->render[i].hash == hash)
			return &db->render[i];
	return NULL;
}

// One pick from a list of `n`: bRandom(n) when `always` or n >= 2, else 0.
static u32 femagPick(const u32* list, u32 n, bool always, u32 (*rnd)(u32 n)) {
	const u32 i = (always ? n != 0 : n >= 2) ? rnd(n) : 0;
	return list[i];
}

void femag_generate_selection(const FeMagSetup* m, FeMagSelection* sel, u32 (*rnd)(u32 n)) {
	sel->back = femagPick(m->back, m->nBack, false, rnd);
	sel->mast = femagPick(m->mast, m->nMast, false, rnd);
	sel->chick = femagPick(m->chick, m->nChick, false, rnd);
	sel->extra = femagPick(m->extra, m->nExtra, false, rnd);
	sel->car = femagPick(m->car, m->nCar, true, rnd);
	sel->render = femagPick(m->render, m->nRender, true, rnd);
}

void magtex_free(MagTex* t) {
	if (t->valid)
		C3D_TexDelete(&t->tex);
	memset(t, 0, sizeof(*t));
}

// The leaf `id` inside the chunk run [p, end), containers (bit 31) entered.
static const u8* find_chunk(const u8* p, const u8* end, u32 id, u32* outLen) {
	while (p + 8 <= end) {
		const u32 cid = rd32(p), len = rd32(p + 4);
		const u8* body = p + 8;
		const u8* next = (len > (u32)(end - body)) ? end : body + len;
		if (cid == id) {
			*outLen = (u32)(next - body);
			return body;
		}
		if (cid & 0x80000000u) {
			const u8* r = find_chunk(body, next, id, outLen);
			if (r)
				return r;
		}
		p = next;
	}
	return NULL;
}

#define MAGTEX_HEAD_BYTES 0x2000
#define MAGTEX_INFO_SIZE  156
#define MAGTEX_ENTRY_SIZE 24

bool magtex_load(MagTex* t, const char* path, u32 hash, int maxDim) {
	magtex_free(t);
	if (!hash)
		return false;
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	// The index is in the pack's first container, inside its first 8 KB: LOW's
	// ends at 0x45C (28 entries, the 14 magazines' two mastheads), HIGH's at
	// 0x105C (124 entries).
	static u8 head[MAGTEX_HEAD_BYTES];
	const size_t got = fread(head, 1, sizeof(head), f);
	u32 len = 0;
	const u8* idx = find_chunk(head, head + got, 0x33310003u, &len);
	u32 off = 0, csize = 0;
	for (u32 k = 0; idx && k + MAGTEX_ENTRY_SIZE <= len; k += MAGTEX_ENTRY_SIZE) {
		if (rd32(idx + k) == hash) {
			off = rd32(idx + k + 4);
			csize = rd32(idx + k + 8);
			break;
		}
	}
	u8* comp = csize ? (u8*)malloc(csize) : NULL;
	const bool read = comp && fseek(f, (long)off, SEEK_SET) == 0 && fread(comp, 1, csize, f) == csize;
	fclose(f);
	if (!read) {
		free(comp);
		return false;
	}
	u8* data = NULL;
	u32 dataLen = 0, consumed = 0;
	const bool unpacked = jdlz_decompress(comp, csize, 0, &data, &dataLen, &consumed);
	free(comp);
	if (!unpacked || dataLen < MAGTEX_INFO_SIZE) {
		free(data);
		return false;
	}
	const u8* info = data + dataLen - MAGTEX_INFO_SIZE;
	TexPackEntry e;
	memset(&e, 0, sizeof(e));
	e.hash = hash;
	e.width = (u16)(info[68] | (info[69] << 8));
	e.height = (u16)(info[70] | (info[71] << 8));
	// HIGH's five D3DFMT_P8 ones (STF_ANNA_01, STF_JAMIE_01, SS_BG_01..03)
	// carry their 256-entry BGRA CLUT between the pixels and the info, the
	// info's +0x3C giving its 0x400 bytes.
	const bool dxt1 = memcmp(info + 144, "DXT1", 4) == 0;
	const bool dxt3 = memcmp(info + 144, "DXT3", 4) == 0;
	const bool p8 = rd32(info + 144) == 0x29u && rd32(info + 0x3C) == 0x400u;
	e.format = dxt1 ? TEXFMT_DXT1 : p8 ? TEXFMT_P8 : TEXFMT_DXT3;
	e.pixelData = data;
	e.pixelLen = (u32)e.width * e.height / (dxt1 ? 2u : 1u);
	if (p8)
		e.palette = data + e.pixelLen;
	bool ok = (dxt1 || dxt3 || p8) && e.width && e.height &&
		e.pixelLen + (p8 ? 0x400u : 0u) <= dataLen - MAGTEX_INFO_SIZE &&
		texpack_decode_entry(&e, maxDim, false, &t->tex, &t->subtex);
	free(data);
	if (!ok)
		return false;
	t->image.tex = &t->tex;
	t->image.subtex = &t->subtex;
	t->hash = hash;
	t->valid = true;
	return true;
}

bool magtex_from_bgra(MagTex* t, const u8* bgra, int w, int h, u32 hash, int maxDim) {
	magtex_free(t);
	if (!bgra || w <= 0 || h <= 0)
		return false;
	TexPackEntry e;
	memset(&e, 0, sizeof(e));
	e.hash = hash;
	e.width = (u16)w;
	e.height = (u16)h;
	e.format = TEXFMT_RAW_BGRA8888;
	e.pixelData = bgra;
	e.pixelLen = (u32)w * (u32)h * 4u;
	if (!texpack_decode_entry(&e, maxDim, false, &t->tex, &t->subtex))
		return false;
	t->image.tex = &t->tex;
	t->image.subtex = &t->subtex;
	t->hash = hash;
	t->valid = true;
	return true;
}
