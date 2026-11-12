// The in-game NIS player -- see nis.h (TASKS.md #343).
#include "nis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NIS_SCENE_HEADER   0x00037030u
#define NIS_CAMERA_SET     0x8003B201u
#define NIS_KEY_SIZE       0x88
#define NIS_FPS            30.0f      // DAT_80319220
#define NIS_LENS_K         15.96f     // DAT_8031BADC / DAT_8031BAE4
#define NIS_LENS_DELTA     0.01f      // DAT_8031BAE8
#define NIS_FOV_DELTA      100.0f     // DAT_8031BAF8
#define NIS_LETTERBOX_UNIT 0.00125f   // DAT_8031C1B0

static uint32_t rd32(const uint8_t* p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static float rdf(const uint8_t* p) {
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

uint32_t nis_hash(const char* s) {
	uint32_t h = 0xFFFFFFFFu;
	while (*s)
		h = h * 33u + (uint8_t)*s++;
	return h;
}

// The 0x37030 header anywhere in [p, end), containers (bit 31) walked into.
static const uint8_t* findChunk(const uint8_t* p, const uint8_t* end, uint32_t want) {
	while (p + 8 <= end) {
		const uint32_t id = rd32(p), size = rd32(p + 4);
		const uint8_t* body = p + 8;
		if (size > (uint32_t)(end - body))
			return NULL;
		if (id == want)
			return p;
		if (id & 0x80000000u) {
			const uint8_t* r = findChunk(body, body + size, want);
			if (r)
				return r;
		}
		p = body + size;
	}
	return NULL;
}

static uint8_t* readFile(const char* path, long* len) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* b = *len > 0 ? malloc((size_t)*len) : NULL;
	if (b && fread(b, 1, (size_t)*len, f) != (size_t)*len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

// The camera set is a top-level chunk of a 370 KB bundle: walked by header,
// and only it read.
static uint8_t* readTopChunk(const char* path, uint32_t want, uint32_t* outSize) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t h[8];
	uint8_t* r = NULL;
	while (fread(h, 1, 8, f) == 8) {
		const uint32_t id = rd32(h), size = rd32(h + 4);
		if (id == want) {
			r = malloc(size ? size : 1);
			if (r && fread(r, 1, size, f) != size) {
				free(r);
				r = NULL;
			}
			*outSize = size;
			break;
		}
		if (fseek(f, (long)size, SEEK_CUR) != 0)
			break;
	}
	fclose(f);
	return r;
}

uint8_t* nis_read_chunk(const char* path, uint32_t want, uint32_t* size) {
	*size = 0;
	return readTopChunk(path, want, size);
}

static void parseKey(NisCamKey* k, const uint8_t* p) {
	memcpy(k->flags, p, 4);
	k->space[0] = p[4];
	k->space[1] = p[5];
	k->t = rdf(p + 0xC);
	for (int i = 0; i < 2; i++) {
		k->tan[i] = rdf(p + 0x10 + 4 * i);
		k->lens[i] = rdf(p + 0x48 + 4 * i);
		k->focal[i] = rdf(p + 0x68 + 4 * i);
		k->aperture[i] = p[0x70 + i];
		k->letterbox[i] = p[0x72 + i];
		for (int c = 0; c < 3; c++) {
			k->eye[i][c] = rdf(p + 0x18 + 12 * i + 4 * c);
			k->look[i][c] = rdf(p + 0x30 + 12 * i + 4 * c);
		}
	}
}

// The header fields out of a 0x37030 chunk (its 8-byte header first).
static bool takeHeader(NisScene* s, const uint8_t* hdr) {
	if (!hdr || rd32(hdr + 4) < 0x34)
		return false;
	hdr += 8;
	s->hash = rd32(hdr);
	memcpy(s->name, hdr + 4, sizeof(s->name) - 1);
	s->type = (int)rd32(hdr + 0x18);
	s->frames = (int)rd32(hdr + 0x28);
	s->length = (float)s->frames / NIS_FPS;
	// CAnimScene::BindToGame (SpeedPS.elf 0x8004BBB8): the StartCountdown
	// marker at +0x30 and, for a scene with property 0, the countdown's GO at
	// +0x2C, both in 30 Hz frames.
	s->goFrames = (int)rd32(hdr + 0x2C);
	s->countdownFrames = (int)rd32(hdr + 0x30);
	return true;
}

// DragCameraGroup::GetTrack(track) out of the camera set: the group of the
// scene's hash, its tracks back to back from +0x10, 0x20 + keys x 0x88 each.
static bool takeTrack(NisScene* s, const char* camBun, int track, void (*log)(const char*)) {
	char b[192];
	uint32_t size = 0;
	uint8_t* cs = readTopChunk(camBun, NIS_CAMERA_SET, &size);
	bool found = false;
	for (uint32_t p = 0; cs && !found && p + 0x10 <= size;) {
		const uint32_t sub = rd32(cs + p + 4);
		const uint32_t n = rd32(cs + p + 0xC);
		if (rd32(cs + p + 8) == s->hash) {
			// #523: the track is the one NAMED "Track <n>" (+0x12), not the
			// n-th: IntroNis10's only track is "Track 1" (candidate 17 asks
			// for 1), IntroNis26's are "Track 2" then "Track 1" (candidates
			// 39 and 40). Everywhere else the name and the position agree.
			// With no such name, the n-th as before.
			char want[14];
			snprintf(want, sizeof(want), "Track %d", track);
			const uint8_t* t = NULL;
			const uint8_t* nth = NULL;
			const uint8_t* q = cs + p + 0x10;
			for (uint32_t k = 0; k < n && q + 0x20 <= cs + size; k++) {
				if (!t && strncmp((const char*)q + 0x12, want, 14) == 0)
					t = q;
				if ((int)k == track)
					nth = q;
				q += 0x20 + (int8_t)q[0x10] * NIS_KEY_SIZE;
			}
			if (!t)
				t = nth;
			if (!t) {
				p += 8 + sub;
				continue;
			}
			const int cnt = t + 0x20 <= cs + size ? (int8_t)t[0x10] : 0;
			if (cnt > 0 && t + 0x20 + cnt * NIS_KEY_SIZE <= cs + size) {
				s->keyCount = cnt > NIS_MAX_KEYS ? NIS_MAX_KEYS : cnt;
				for (int k = 0; k < s->keyCount; k++)
					parseKey(&s->key[k], t + 0x20 + k * NIS_KEY_SIZE);
				found = true;
			}
		}
		p += 8 + sub;
	}
	free(cs);
	if (!found) {
		snprintf(b, sizeof(b), "nis: no camera group 0x%08X (%s) track %d in %s", (unsigned)s->hash,
			s->name, track, camBun);
		log(b);
		return false;
	}
	s->track = track;
	s->loaded = true;
	snprintf(b, sizeof(b), "nis: %s loaded -- hash 0x%08X, type %d, %d frames (%.1f s), track %d, %d camera keys",
		s->name, (unsigned)s->hash, s->type, s->frames, s->length, track, s->keyCount);
	log(b);
	return true;
}

static void sceneIdentity(NisScene* s) {
	memset(s->scene, 0, sizeof(s->scene));
	s->scene[0][0] = s->scene[1][1] = s->scene[2][2] = 1.0f;
}

bool nis_load(NisScene* s, const char* sceneBun, const char* camBun, void (*log)(const char*)) {
	char b[192];
	memset(s, 0, sizeof(*s));
	sceneIdentity(s);
	long len = 0;
	uint8_t* sb = readFile(sceneBun, &len);
	const bool ok = sb && takeHeader(s, findChunk(sb, sb + len, NIS_SCENE_HEADER));
	free(sb);
	if (!ok) {
		snprintf(b, sizeof(b), "nis: %s %s", sceneBun, sb ? "has no scene header (0x37030)" : "missing");
		log(b);
		return false;
	}
	// "Track 0" -- the scene's own (the track number at r13-0x7E18 is 0 for a
	// shop NIS).
	return takeTrack(s, camBun, 0, log);
}

bool nis_load_mem(NisScene* s, const void* chunk, uint32_t bytes, const char* camBun, int track,
	void (*log)(const char*))
{
	memset(s, 0, sizeof(*s));
	sceneIdentity(s);
	const uint8_t* p = (const uint8_t*)chunk;
	if (!p || !takeHeader(s, findChunk(p, p + bytes, NIS_SCENE_HEADER))) {
		log("nis: the scene chunk has no scene header (0x37030)");
		return false;
	}
	return takeTrack(s, camBun, track, log);
}

void nis_set_scene_matrix(NisScene* s, const float m[4][3]) {
	memcpy(s->scene, m, sizeof(s->scene));
}

// A Scene-space (3) point: the cubic's value times the scene matrix (row
// vectors; CAnimScene +0xC4, applied where the mover sets its desired eye and
// look). Car and Hybrid are still taken as World.
static void toWorld(const NisScene* s, int space, float v[3]) {
	if (space != 3)
		return;
	float o[3];
	for (int c = 0; c < 3; c++)
		o[c] = v[0] * s->scene[0][c] + v[1] * s->scene[1][c] + v[2] * s->scene[2][c] + s->scene[3][c];
	memcpy(v, o, sizeof(o));
}

// tCubic1D::MakeCoeffs and GetVal: Hermite from (p0, d0) to (p1, d1).
static float cubic(float p0, float d0, float p1, float d1, float t) {
	const float dl = p1 - p0;
	const float a = d0 + d1 - (dl + dl);
	const float b = dl * 3.0f - d1 - (d0 + d0);
	return ((a * t + b) * t + d0) * t + p0;
}

// bATan(x, y) as a bAngle, 65536 to the turn.
static int bATan(float x, float y) {
	return (int)(atan2f(y, x) * (65536.0f / (2.0f * (float)M_PI))) & 0xFFFF;
}

static int lensToFov(float lens) {            // 0x80069F78
	return (bATan(lens, NIS_LENS_K) & 0x7FFF) << 1;
}

static float lensDeltaToFovDelta(float lens, float d) {   // 0x80069FF8
	const int16_t s0 = (int16_t)(bATan(lens, NIS_LENS_K) << 1);
	const int16_t s1 = (int16_t)(bATan(d * NIS_LENS_DELTA + lens, NIS_LENS_K) << 1);
	return (float)(s1 - s0) * NIS_FOV_DELTA;
}

void nis_view(const NisScene* s, float t, NisView* v) {
	memset(v, 0, sizeof(*v));
	if (!s->loaded || s->keyCount <= 0)
		return;
	const float param = s->length > 0.0f ? t / s->length : 1.0f;
	// GetKeyNumber: the last key that has started; its interval runs to the
	// next one's start, the last's to 1.
	int k = 0;
	while (k + 1 < s->keyCount && s->key[k + 1].t <= param)
		k++;
	const NisCamKey* key = &s->key[k];
	const float end = k + 1 < s->keyCount ? s->key[k + 1].t : 1.0f;
	float f = end > key->t ? (param - key->t) / (end - key->t) : 1.0f;
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	v->key = k;
	// GetSlope, unshared: (end - start) x tangent scale, none when the key is
	// not smooth.
	float sl[2];
	for (int i = 0; i < 2; i++)
		sl[i] = key->flags[0] ? key->tan[i] : 0.0f;
	for (int c = 0; c < 3; c++) {
		const float de = key->eye[1][c] - key->eye[0][c], dk = key->look[1][c] - key->look[0][c];
		v->eye[c] = key->flags[2]
			? cubic(key->eye[0][c], de * sl[0], key->eye[1][c], de * sl[1], f)
			: key->eye[0][c] * (1.0f - f) + key->eye[1][c] * f;
		v->look[c] = key->flags[3]
			? cubic(key->look[0][c], dk * sl[0], key->look[1][c], dk * sl[1], f)
			: key->look[0][c] * (1.0f - f) + key->look[1][c] * f;
	}
	// The FOV cubic over bAngles, GetFOV's & 0xFFFF.
	const float dl = key->lens[1] - key->lens[0];
	const float fov0 = (float)lensToFov(key->lens[0]), fov1 = (float)lensToFov(key->lens[1]);
	float fov = key->flags[2]
		? cubic(fov0, lensDeltaToFovDelta(key->lens[0], dl * sl[0]),
			fov1, lensDeltaToFovDelta(key->lens[1], dl * sl[1]), f)
		: fov0 * (1.0f - f) + fov1 * f;
	toWorld(s, key->space[0], v->eye);
	toWorld(s, key->space[1], v->look);
	v->fovDeg = (float)((int)fov & 0xFFFF) * (360.0f / 65536.0f);
	// The letterbox cubic, its slopes left at 0 (the ctor's DAT_8031C140).
	v->letterbox = cubic((float)key->letterbox[0], 0.0f, (float)key->letterbox[1], 0.0f, f) *
		NIS_LETTERBOX_UNIT;
	// The focal distance and aperture cubics (SetDesired's mover +0x48 and
	// +0x4C, from the key's +0x68 floats and +0x70 bytes), the same way.
	v->focal = cubic(key->focal[0], 0.0f, key->focal[1], 0.0f, f);
	v->aperture = cubic((float)key->aperture[0], 0.0f, (float)key->aperture[1], 0.0f, f);
}

// IceCameraMover::Update (SpeedPS.elf 0x800753B0, its tail): the lens length
// in mm (ConvertFovAngleToLensLength 0x80069FA4, 15.96 over the half-angle's
// tangent), the f-stop (ConvertApertureNumberToFStop 0x8006A098: a table of
// 2^(n/6), n the aperture rounded into 0..36), the hyperfocal distance
// H = L^2 / (N x 0.03 mm) + L, and the near and far limits of focus about
// the focal distance (at least 0.01 m) -- far "infinite", 100 km, once the
// focus is past H. The camera keeps their middle and their span.
bool nis_dof(const NisView* v, float* focus, float* span) {
	*focus = 0.0f;
	*span = 0.0f;
	if (!v || v->aperture >= 37.0f || v->fovDeg <= 0.0f)
		return false;
	float fd = v->focal;
	if (fd < 0.01f)
		fd = 0.01f;
	fd *= 1000.0f;
	const float L = NIS_LENS_K / tanf(v->fovDeg * ((float)M_PI / 360.0f));
	int n = (int)(v->aperture + 0.5f);
	if (n < 0) n = 0;
	if (n > 36) n = 36;
	const float N = powf(2.0f, (float)n / 6.0f);
	const float H = L * L / (N * 0.03f) + L;
	const float num = (H - L) * fd;
	const float nearM = num / (H + fd - (L + L)) / 1000.0f;
	const float farM = H - fd > 1e-5f ? num / (H - fd) / 1000.0f : 100000.0f;
	*focus = (farM + nearM) * 0.5f;
	*span = farM - nearM;
	return true;
}
