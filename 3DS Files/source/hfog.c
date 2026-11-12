// See hfog.h. The addresses are SPEED2.EXE's.
#include "hfog.h"
#include "shaders.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HFOG_CHUNK     0x34250u
#define HFOG_VERSION   2
#define HFOG_ENTRY     100       // one light, 0x64 bytes
#define HFOG_TYPE      6         // the list at 0x88f1f8 (0x88f1c8 + type * 8)
#define HFOG_MAX       64

// FUN_005C7470's grid: 10 x 10 vertices, row 0 at the bottom of the screen.
#define HFOG_N         10
// Rows 0-4 are alpha 0 (row r's is (r * 0.1 - 0.4) * 425 when that is
// positive, truncated), so the strips under row 4 add nothing.
#define HFOG_ROW0      4
static const u8 kRowAlpha[HFOG_N] = { 0, 0, 0, 0, 0, 42, 85, 127, 170, 212 };
// Row r's z is 0.9995 - r * 0.0001, through the player view's projection
// (CreateViewMatricies 0x5C7EC0: near camera+0xBC, 0.5, and far 15000):
// D3D's z = f / (f - n) * (1 - n / d), so d = n / (1 - z * (f - n) / f).
#define HFOG_PC_NEAR   0.5f
#define HFOG_PC_FAR    15000.0f
// PORT: in stereo each eye's frustum is sheared a little past the centre
// view's, so the edge columns are laid that much wider. Their colours are
// still the screen edge's.
#define HFOG_EDGE      1.05f

// FUN_005DBB00 works in the PC's pixels, and the glow's reach (128 / 0.45,
// 284) with them, at 640x480 by default (g_RacingResolution 0).
// PORT: the top screen is 5:3, so the same 480 lines are laid 800 wide,
// which keeps the glow as round on the screen and as tall as on the PC.
#define HFOG_VH        480
#define HFOG_VW        800

#define HFOG_QUADS     ((HFOG_N - 1 - HFOG_ROW0) * (HFOG_N - 1))
#define HFOG_VERTS     (HFOG_QUADS * 6)
// One grid a frame, drawn by both eyes; never rewritten while a frame the
// GPU may still be reading holds it.
#define HFOG_RING      4

typedef struct {
	float pos[3];
	float rgb[3];
	float nearD, farD;    // +0x34, +0x2C
	// FUN_005DB900's, every frame
	float sx, sy;         // +0x44, +0x48
	float fade;           // +0x54
	float dist;           // the sort's key
} HfogLight;

typedef struct {
	float pos[3];
	u8    rgba[4];
	float uv[2];
} HfogVert;

static void (*s_log)(const char*);
static HfogLight s_l[HFOG_MAX];
static int s_n;
static HfogVert* s_vbuf;
static int s_slot;
static int s_nv;          // this frame's, 0 when nothing glows
static int s_lit;         // and its lights in front, for the log
static u8  s_peak[3];     // and the most any vertex adds, colour times alpha

static DVLE_s*         s_dvle;   // the shared blob\'s (shaders.h, #352)
static shaderProgram_s s_prog;
static int             s_uMvp = -1;
static bool            s_shader;

static void hlog(const char* m) {
	if (s_log)
		s_log(m);
}

static u32 rd32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static float rdf(const u8* p) {
	const u32 v = rd32(p);
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

static bool initShader(void) {
	if (s_shader)
		return true;
	s_dvle = shaders_get(SHADER_PARTICLE);
	if (!s_dvle)
		return false;
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, s_dvle);
	s_uMvp = shaderInstanceGetUniformLocation(s_prog.vertexShader, "mvp");
	if (s_uMvp < 0) {
		shaderProgramFree(&s_prog);
		s_dvle = NULL;
		return false;
	}
	s_shader = true;
	return true;
}

// The bundle's chunks walked flat: a container's (bit 31) children follow
// its header. The payload starts on the next 16 bytes past the header, as
// the loader takes it (0x5E0190: (chunk + 0x17) & ~0xF).
static int readLights(FILE* f, long size) {
	long pos = 0;
	u8 h[8];
	while (pos + 8 <= size) {
		if (fseek(f, pos, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8)
			return -1;
		const u32 id = rd32(h), len = rd32(h + 4);
		if (id == HFOG_CHUNK) {
			const long end = pos + 8 + (long)len;
			const long body = (pos + 0x17) & ~0xFL;
			if (end > size || body + 0x10 > end)
				return -1;
			u8* b = (u8*)malloc((size_t)(end - body));
			if (!b)
				return -1;
			int n = -1;
			if (fseek(f, body, SEEK_SET) == 0 &&
					fread(b, 1, (size_t)(end - body), f) == (size_t)(end - body)) {
				const u32 ver = rd32(b + 8), cnt = rd32(b + 12);
				if (ver == HFOG_VERSION && 0x10 + (u64)cnt * HFOG_ENTRY <= (u64)(end - body)) {
					n = 0;
					for (u32 i = 0; i < cnt && n < HFOG_MAX; i++) {
						const u8* e = b + 0x10 + i * HFOG_ENTRY;
						if (rd32(e + 0x08) != HFOG_TYPE)
							continue;
						HfogLight* L = &s_l[n++];
						memset(L, 0, sizeof(*L));
						for (int k = 0; k < 3; k++) {
							L->rgb[k] = rdf(e + 0x0C + 4 * k);
							L->pos[k] = rdf(e + 0x20 + 4 * k);
						}
						L->farD = rdf(e + 0x2C);
						L->nearD = rdf(e + 0x34);
					}
				}
			}
			free(b);
			return n;
		}
		pos += (id & 0x80000000u) ? 8 : 8 + (long)len;
	}
	return 0;
}

bool hfog_open(const char* bunPath, void (*log)(const char*)) {
	hfog_close();
	s_log = log;
	char b[200];
	FILE* f = bunPath ? fopen(bunPath, "rb") : NULL;
	if (!f) {
		hlog("hfog: no L4RA.BUN -- no horizon fog");
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	const int n = readLights(f, size);
	fclose(f);
	if (n <= 0) {
		snprintf(b, sizeof(b), "hfog: %s in L4RA.BUN -- no horizon fog",
			n < 0 ? "chunk 0x34250 malformed" : "no type-6 lights");
		hlog(b);
		s_n = 0;
		return false;
	}
	if (!initShader() ||
			!(s_vbuf = (HfogVert*)linearAlloc(HFOG_RING * HFOG_VERTS * sizeof(HfogVert)))) {
		hlog("hfog: no shader or vertex memory -- no horizon fog");
		s_n = 0;
		return false;
	}
	s_n = n;
	snprintf(b, sizeof(b), "hfog: %d lights (chunk 0x34250)", n);
	hlog(b);
	return true;
}

void hfog_close(void) {
	s_n = 0;
	s_nv = 0;
	if (s_vbuf)
		linearFree(s_vbuf);
	s_vbuf = NULL;
}

bool hfog_loaded(void) { return s_n > 0 && s_vbuf; }

static int ftolClamp(float v) {
	return v > 1e6f ? 1000000 : v < -1e6f ? -1000000 : (int)v;
}

// FUN_005DBB00: grid vertex (px, py)'s colour, from the lights in `order`.
// Each light within 284 pixels adds its colour in proportion to how near it
// is, nearest light first, each into what the ones before left uncovered.
static void vertexColour(int px, int py, const int* order, float intensity, u8 out[3]) {
	float R = 0, G = 0, B = 0, A = 0;
	for (int k = 0; k < s_n; k++) {
		const HfogLight* L = &s_l[order[k]];
		if (!(L->fade > 0.0f))
			continue;
		const int dx = abs(px - ftolClamp(L->sx)), dy = abs(py - ftolClamp(L->sy));
		const int d = dx > dy ? dx + (dy >> 2) : dy + (dx >> 2);
		float t = (128.0f - (float)d * 0.45f) * (1.0f / 128.0f);
		if (t < 0.0f)
			continue;
		const float w = 1.0f - A;
		t *= L->fade;
		if (w >= 0.0f) {
			R += L->rgb[0] * 0.5f * t * w;
			G += L->rgb[1] * 0.5f * t * w;
			B += L->rgb[2] * 0.5f * t * w;
		}
		A += t;
	}
	if (A > 0.0f) {
		const float k = 1.8f * intensity * 1.8f;
		R *= k; G *= k; B *= k;
		if (R > 255.0f) R = 255.0f;
		if (G > 255.0f) G = 255.0f;
		if (B > 255.0f) B = 255.0f;
	}
	out[0] = (u8)R;
	out[1] = (u8)G;
	out[2] = (u8)B;
}

void hfog_update(const HfogView* v, float intensity) {
	s_nv = 0;
	s_lit = 0;
	memset(s_peak, 0, sizeof(s_peak));
	if (!hfog_loaded() || !v || v->tanX <= 0.0f || v->tanY <= 0.0f)
		return;
	// FUN_005DB900: fade and screen place, then nearest first (FUN_005D74A0).
	int order[HFOG_MAX];
	bool any = false;
	for (int i = 0; i < s_n; i++) {
		HfogLight* L = &s_l[i];
		float d[3] = { L->pos[0] - v->eye[0], L->pos[1] - v->eye[1], L->pos[2] - v->eye[2] };
		L->dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		const float z = d[0] * v->fwd[0] + d[1] * v->fwd[1] + d[2] * v->fwd[2];
		const float dot = L->dist > 0.0f ? z / L->dist : 0.0f;
		if (dot > 0.0f) {
			L->fade = L->dist < L->nearD ? 0.0f
			        : L->dist >= L->farD ? 1.0f
			        : (L->dist - L->nearD) / (L->farD - L->nearD);
			// FUN_005BC4A0: x = (ndc_x + 1) * W / 2, y = (1 - ndc_y) * H / 2.
			const float nx = (d[0] * v->right[0] + d[1] * v->right[1] + d[2] * v->right[2]) /
				(z * v->tanX);
			const float ny = (d[0] * v->up[0] + d[1] * v->up[1] + d[2] * v->up[2]) /
				(z * v->tanY);
			L->sx = (nx + 1.0f) * (HFOG_VW * 0.5f);
			L->sy = (1.0f - ny) * (HFOG_VH * 0.5f);
		} else {
			L->fade = dot;
		}
		if (L->fade > 0.0f) {
			any = true;
			s_lit++;
		}
		int j = i;
		while (j > 0 && s_l[order[j - 1]].dist > L->dist) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = i;
	}
	if (!any || intensity <= 0.0f)
		return;

	// FUN_005C7470's grid: colours sampled at whole-pixel steps (W / 9, H / 9
	// truncated), positions laid back into the world at each row's depth
	// along the centre view's rays.
	static float pos[HFOG_N][HFOG_N][3];
	static u8 col[HFOG_N][HFOG_N][3];
	bool lit = false;
	for (int r = HFOG_ROW0; r < HFOG_N; r++) {
		const float zn = 0.9995f - (float)r * 0.0001f;
		const float depth = HFOG_PC_NEAR /
			(1.0f - zn * ((HFOG_PC_FAR - HFOG_PC_NEAR) / HFOG_PC_FAR));
		const float yn = (float)r * (2.0f / 9.0f) - 1.0f;
		for (int c = 0; c < HFOG_N; c++) {
			float xn = (float)c * (2.0f / 9.0f) - 1.0f;
			if (c == 0 || c == HFOG_N - 1)
				xn *= HFOG_EDGE;
			for (int k = 0; k < 3; k++)
				pos[r][c][k] = v->eye[k] + depth * (v->fwd[k] + xn * v->tanX * v->right[k] +
					yn * v->tanY * v->up[k]);
			vertexColour(c * (HFOG_VW / 9), (HFOG_N - 1 - r) * (HFOG_VH / 9), order,
				intensity, col[r][c]);
			if (kRowAlpha[r] && (col[r][c][0] | col[r][c][1] | col[r][c][2]))
				lit = true;
			for (int k = 0; k < 3; k++) {
				const int a = col[r][c][k] * kRowAlpha[r] / 255;
				if (a > s_peak[k])
					s_peak[k] = (u8)a;
			}
		}
	}
	if (!lit)
		return;
	HfogVert* const vb = s_vbuf + s_slot * HFOG_VERTS;
	HfogVert* q = vb;
	static const int kr[6] = { 0, 0, 1, 0, 1, 1 }, kc[6] = { 0, 1, 1, 0, 1, 0 };
	for (int r = HFOG_ROW0; r < HFOG_N - 1; r++) {
		for (int c = 0; c < HFOG_N - 1; c++) {
			for (int i = 0; i < 6; i++, q++) {
				const int rr = r + kr[i], cc = c + kc[i];
				memcpy(q->pos, pos[rr][cc], sizeof(q->pos));
				memcpy(q->rgba, col[rr][cc], 3);
				q->rgba[3] = kRowAlpha[rr];
				q->uv[0] = (float)cc / 9.0f;
				q->uv[1] = (float)rr / 9.0f;
			}
		}
	}
	s_nv = (int)(q - vb);
	GSPGPU_FlushDataCache(vb, (u32)s_nv * sizeof(HfogVert));
	s_slot = (s_slot + 1) % HFOG_RING;
}

int hfog_stats(u8 peak[3]) {
	if (peak)
		memcpy(peak, s_peak, sizeof(s_peak));
	return s_lit;
}

void hfog_draw(const C3D_Mtx* vp) {
	if (!s_nv || !s_shader)
		return;
	HfogVert* const vb = s_vbuf + ((s_slot + HFOG_RING - 1) % HFOG_RING) * HFOG_VERTS;
	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);           // v0 position
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);   // v1 colour
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);           // v2 texcoord
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, vb, sizeof(HfogVert), 3, 0x210);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uMvp, vp);

	// The pass's FX states: no cull, no alpha test, z tested and not written,
	// RGB only (COLORWRITE 7), dst += src * srcAlpha. No texture (stage 0's
	// is NULL), so the vertex colour as it is.
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
	C3D_DrawArrays(GPU_TRIANGLES, 0, s_nv);

	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
		GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}
