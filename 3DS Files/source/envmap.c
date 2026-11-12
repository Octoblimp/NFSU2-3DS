// See envmap.h. TASKS.md #166 has the trace; the GameCube functions named
// here are Speed.elf's.
#include "envmap.h"

#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model3d.h"
#include "shaders.h"
#include "gpuwatch.h"

static void (*s_kick)(void);

void envmap_set_kick(void (*kick)(void)) {
	s_kick = kick;
}

#define ENV_FACES    6
#define ENV_FACE_SZ  64     // eInitEnvMap's capture buffers
#define ENV_SPHERE   128    // cSphereMap::Init's
// The face cameras: 90 degrees square, near 0.5, far 10000 (the eViews
// UpdateEnvironmentMapCameras sets up).
#define ENV_NEAR     0.5f
#define ENV_FAR      10000.0f
// The disc mesh: rings evenly spaced in the normal's angle from the centre,
// so the reflected direction moves at most ~6 degrees across a triangle.
#define ENV_RINGS    32
#define ENV_SEGS     64
#define ENV_VERTS    (1 + ENV_RINGS * ENV_SEGS)

// The faces in the env camera's frame (x right, y up, z back toward the world
// camera -- its LookAt's rows): each one's forward and up, as
// UpdateEnvironmentMapCameras turns them. Face 0 looks back at the world
// camera, 2 ahead, 4 up and 5 down.
static const float kFaceF[ENV_FACES][3] = {
	{ 0, 0, 1 }, { 1, 0, 0 }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 },
};
static const float kFaceU[ENV_FACES][3] = {
	{ 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 },
};

// gpuwatch_trace()'s names.
static const char* const kTraceClear[ENV_FACES] = {
	"env face 0: clear", "env face 1: clear", "env face 2: clear",
	"env face 3: clear", "env face 4: clear", "env face 5: clear",
};
static const char* const kTraceDraw[ENV_FACES] = {
	"env face 0: sections", "env face 1: sections", "env face 2: sections",
	"env face 3: sections", "env face 4: sections", "env face 5: sections",
};

// Schedule mode 0: which faces each frame of four renders.
static const u8 kSchedule0[4][ENV_FACES] = {
	{ 1, 1, 0, 0, 1, 0 },
	{ 1, 0, 1, 0, 1, 0 },
	{ 1, 0, 0, 1, 1, 0 },
	{ 1, 0, 0, 0, 1, 1 },
};

// Schedule mode 3, the 3DS's (#432): two faces a frame round three, paired
// by cost, so every face is a frame in three. The GameCube's mode 0 draws
// face 0 (back toward the camera) and face 4 (up) every frame; on Azahar
// face 0 is the costliest face (8.3k triangles at J29.1) and, through the
// cars' Fresnel lerp, worth 6-22% of their reflection; face 4 drew nothing
// the cars showed at D24 or J29.1. Face 2 (ahead), 40% at J29.1, goes from
// one frame in four to one in three. J29.1: mean 13.6k -> 9.4k triangles a
// frame, the worst 18.9k -> 10.4k.
static const u8 kSchedule3[3][ENV_FACES] = {
	{ 1, 0, 0, 0, 1, 0 },
	{ 0, 0, 1, 0, 0, 1 },
	{ 0, 1, 0, 1, 0, 0 },
};

typedef struct {
	float x, y;   // clip space
	float s, t;   // the face texture's
	float a;      // the face's weight
} EnvVert;

struct EnvMap {
	C3D_Tex           face[ENV_FACES];
	C3D_RenderTarget* faceTarget[ENV_FACES];
	bool              faceDrawn[ENV_FACES];
	C3D_Tex           sphere;
	C3D_RenderTarget* sphereTarget;
	DVLE_s*           dvle;   // the shared blob's (shaders.h, #352)
	shaderProgram_s   prog;
	EnvVert*          verts[ENV_FACES];
	u16*              idx[ENV_FACES];
	int               nIdx[ENV_FACES];
	EnvVert*          quad;       // envClear()'s, a strip over the whole target
	int               mode;
	unsigned          skip;       // DbgEnvFaceOff
	void            (*log)(const char*);
	u32               faceTris[ENV_FACES];   // each face's, when last drawn
	u64               faceTicks[ENV_FACES];
	u32               frame;
	u32               tris;
	int               faces;
	float             ax[3][3];   // the env camera's right, up and back
	bool              turned;     // what the faces hold (envmap_render_with)
};

static void cross3(float o[3], const float a[3], const float b[3]) {
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot3(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool norm3(float v[3]) {
	const float l = sqrtf(dot3(v, v));
	if (l < 1e-6f)
		return false;
	for (int i = 0; i < 3; i++)
		v[i] /= l;
	return true;
}

// cSphereMap::genSphere's disc: the texel at (x, y) is the normal
// n = (x, y, sqrt(1 - x^2 - y^2)) in the env camera's frame, and holds what a
// view down -z reflects off it: r = 2 n.z n - (0, 0, 1), which is
// (2 n.z n.x, 2 n.z n.y, 2 n.z^2 - 1). The centre sees face 0, the rim
// face 2.
//
// PORT: genSphereMap draws the disc six times, each face projected through
// its LightFrustum, its alpha its 1-pixel black border times a light channel
// that is 1 in front of the face, and blends them. The port gives each disc
// vertex to the one face its r falls in and weights each face's pass by that
// (1 or 0 at a vertex), additive over the black clear: the weights sum to 1
// everywhere, so the faces meet without a seam and without needing an alpha
// channel in the faces. The disc is fixed in the env camera's frame, so all
// of this is built once.
static bool buildDisc(EnvMap* e) {
	float (*r)[3] = (float (*)[3])malloc(sizeof(float) * 3 * ENV_VERTS);
	float (*p)[2] = (float (*)[2])malloc(sizeof(float) * 2 * ENV_VERTS);
	u8* owner = (u8*)malloc(ENV_VERTS);
	int* remap = (int*)malloc(sizeof(int) * ENV_VERTS);
	u16 (*tri)[3] = (u16 (*)[3])malloc(sizeof(u16) * 3 * ENV_SEGS * (2 * ENV_RINGS - 1));
	bool ok = r && p && owner && remap && tri;
	int nTri = 0;
	if (ok) {
		for (int v = 0; v < ENV_VERTS; v++) {
			float th = 0.0f, ph = 0.0f;
			if (v) {
				const int ring = 1 + (v - 1) / ENV_SEGS;
				th = (float)ring / ENV_RINGS * ((float)M_PI * 0.5f);
				ph = (float)((v - 1) % ENV_SEGS) / ENV_SEGS * (2.0f * (float)M_PI);
			}
			const float n[3] = { sinf(th) * cosf(ph), sinf(th) * sinf(ph), cosf(th) };
			p[v][0] = n[0];
			p[v][1] = n[1];
			r[v][0] = 2.0f * n[2] * n[0];
			r[v][1] = 2.0f * n[2] * n[1];
			r[v][2] = 2.0f * n[2] * n[2] - 1.0f;
			int best = 0;
			for (int k = 1; k < ENV_FACES; k++)
				if (dot3(r[v], kFaceF[k]) > dot3(r[v], kFaceF[best]))
					best = k;
			owner[v] = (u8)best;
		}
		for (int j = 0; j < ENV_SEGS; j++) {
			const int j1 = (j + 1) % ENV_SEGS;
			tri[nTri][0] = 0;
			tri[nTri][1] = (u16)(1 + j);
			tri[nTri][2] = (u16)(1 + j1);
			nTri++;
			for (int i = 1; i < ENV_RINGS; i++) {
				const int a = 1 + (i - 1) * ENV_SEGS, b = 1 + i * ENV_SEGS;
				tri[nTri][0] = (u16)(a + j);
				tri[nTri][1] = (u16)(b + j);
				tri[nTri][2] = (u16)(b + j1);
				nTri++;
				tri[nTri][0] = (u16)(a + j);
				tri[nTri][1] = (u16)(b + j1);
				tri[nTri][2] = (u16)(a + j1);
				nTri++;
			}
		}
	}
	for (int k = 0; ok && k < ENV_FACES; k++) {
		// The triangles with a vertex of this face's, and their vertices.
		int nv = 0, ni = 0;
		for (int v = 0; v < ENV_VERTS; v++)
			remap[v] = -1;
		for (int t = 0; t < nTri; t++) {
			if (owner[tri[t][0]] != k && owner[tri[t][1]] != k && owner[tri[t][2]] != k)
				continue;
			for (int c = 0; c < 3; c++)
				if (remap[tri[t][c]] < 0)
					remap[tri[t][c]] = nv++;
			ni += 3;
		}
		e->verts[k] = (EnvVert*)linearAlloc(sizeof(EnvVert) * (size_t)(nv ? nv : 1));
		e->idx[k] = (u16*)linearAlloc(sizeof(u16) * (size_t)(ni ? ni : 1));
		if (!e->verts[k] || !e->idx[k]) {
			ok = false;
			break;
		}
		float R[3];
		cross3(R, kFaceF[k], kFaceU[k]);
		for (int v = 0; v < ENV_VERTS; v++) {
			if (remap[v] < 0)
				continue;
			// The face camera's own projection of r: x = r.R / r.F and
			// y = r.U / r.F, at the texel t = 0.5 + 0.5 y (a render target's
			// clip-space +y lands at t = 1, as citro2d reads one). A vertex
			// of another face's is just over this one's edge, and only ever
			// sampled at a fraction of its weight; the edge texels stand in.
			float w = dot3(r[v], kFaceF[k]);
			if (w < 0.05f)
				w = 0.05f;
			float x = dot3(r[v], R) / w, y = dot3(r[v], kFaceU[k]) / w;
			x = x < -2.0f ? -2.0f : x > 2.0f ? 2.0f : x;
			y = y < -2.0f ? -2.0f : y > 2.0f ? 2.0f : y;
			EnvVert* o = &e->verts[k][remap[v]];
			o->x = p[v][0];
			o->y = p[v][1];
			o->s = 0.5f + 0.5f * x;
			o->t = 0.5f + 0.5f * y;
			o->a = owner[v] == k ? 1.0f : 0.0f;
		}
		ni = 0;
		for (int t = 0; t < nTri; t++) {
			if (owner[tri[t][0]] != k && owner[tri[t][1]] != k && owner[tri[t][2]] != k)
				continue;
			for (int c = 0; c < 3; c++)
				e->idx[k][ni++] = (u16)remap[tri[t][c]];
		}
		e->nIdx[k] = ni;
		GSPGPU_FlushDataCache(e->verts[k], sizeof(EnvVert) * (u32)(nv ? nv : 1));
		GSPGPU_FlushDataCache(e->idx[k], sizeof(u16) * (u32)(ni ? ni : 1));
	}
	free(r);
	free(p);
	free(owner);
	free(remap);
	free(tri);
	return ok;
}

void envmap_destroy(EnvMap* e) {
	if (!e)
		return;
	for (int k = 0; k < ENV_FACES; k++) {
		if (e->faceTarget[k])
			C3D_RenderTargetDelete(e->faceTarget[k]);
		if (e->face[k].data)
			C3D_TexDelete(&e->face[k]);
		if (e->verts[k])
			linearFree(e->verts[k]);
		if (e->idx[k])
			linearFree(e->idx[k]);
	}
	if (e->quad)
		linearFree(e->quad);
	if (e->sphereTarget)
		C3D_RenderTargetDelete(e->sphereTarget);
	if (e->sphere.data)
		C3D_TexDelete(&e->sphere);
	if (e->dvle) {
		shaderProgramFree(&e->prog);
	}
	free(e);
}

static void say(void (*log)(const char*), const char* what) {
	if (!log)
		return;
	char b[200];
	snprintf(b, sizeof(b), "envmap: %s", what);
	log(b);
}

EnvMap* envmap_create(void (*log)(const char*)) {
	EnvMap* e = (EnvMap*)calloc(1, sizeof(EnvMap));
	if (!e) {
		say(log, "no memory -- the cars go without their reflection");
		return NULL;
	}
	e->log = log;
	// Looking along +x until a first frame says otherwise.
	e->ax[0][1] = -1.0f;
	e->ax[1][2] = 1.0f;
	e->ax[2][0] = -1.0f;
	const u32 vram0 = vramSpaceFree();
	bool ok = true;
	for (int k = 0; ok && k < ENV_FACES; k++) {
		ok = C3D_TexInitVRAM(&e->face[k], ENV_FACE_SZ, ENV_FACE_SZ, GPU_RGB565);
		if (ok) {
			C3D_TexSetFilter(&e->face[k], GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(&e->face[k], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			e->faceTarget[k] = C3D_RenderTargetCreateFromTex(&e->face[k], GPU_TEXFACE_2D,
				0, GPU_RB_DEPTH24_STENCIL8);
			ok = e->faceTarget[k] != NULL;
		}
	}
	if (ok)
		ok = C3D_TexInitVRAM(&e->sphere, ENV_SPHERE, ENV_SPHERE, GPU_RGB565);
	if (ok) {
		C3D_TexSetFilter(&e->sphere, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&e->sphere, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		e->sphereTarget = C3D_RenderTargetCreateFromTex(&e->sphere, GPU_TEXFACE_2D, 0, -1);
		ok = e->sphereTarget != NULL;
	}
	if (!ok) {
		say(log, "no VRAM for the faces -- the cars go without their reflection");
		envmap_destroy(e);
		return NULL;
	}
	e->dvle = shaders_get(SHADER_ENVMAP);
	if (!e->dvle) {
		say(log, "shader would not load -- the cars go without their reflection");
		envmap_destroy(e);
		return NULL;
	}
	shaderProgramInit(&e->prog);
	shaderProgramSetVsh(&e->prog, e->dvle);
	e->quad = (EnvVert*)linearAlloc(sizeof(EnvVert) * 4);
	if (!e->quad || !buildDisc(e)) {
		say(log, "no linear memory for the disc -- the cars go without their reflection");
		envmap_destroy(e);
		return NULL;
	}
	static const EnvVert kQuad[4] = {
		{ -1.0f, -1.0f, 0, 0, 0 }, { 1.0f, -1.0f, 0, 0, 0 },
		{ -1.0f,  1.0f, 0, 0, 0 }, { 1.0f,  1.0f, 0, 0, 0 },
	};
	memcpy(e->quad, kQuad, sizeof(kQuad));
	GSPGPU_FlushDataCache(e->quad, sizeof(kQuad));
	if (log) {
		char b[200];
		snprintf(b, sizeof(b), "envmap: six %dx%d faces and a %dx%d sphere map, %lu KB VRAM "
			"(%lu KB left); disc %d/%d/%d/%d/%d/%d indices",
			ENV_FACE_SZ, ENV_FACE_SZ, ENV_SPHERE, ENV_SPHERE,
			(unsigned long)((vram0 - vramSpaceFree()) >> 10),
			(unsigned long)(vramSpaceFree() >> 10),
			e->nIdx[0], e->nIdx[1], e->nIdx[2], e->nIdx[3], e->nIdx[4], e->nIdx[5]);
		log(b);
	}
	return e;
}

void envmap_set_schedule(EnvMap* e, int mode) {
	if (e)
		e->mode = mode >= 1 && mode <= 3 ? mode : 0;
}

void envmap_set_face_skip(EnvMap* e, unsigned mask) {
	if (e)
		e->skip = mask & 0x3Fu;
}

void envmap_reset(EnvMap* e) {
	if (!e)
		return;
	for (int k = 0; k < ENV_FACES; k++)
		e->faceDrawn[k] = false;
}

// eInitEnvMap's black clear, drawn: a quad over the whole target writing
// colour 0 and, with `depth`, depth 0 -- the far end the world's GPU_GREATER
// tests against, which C3D_DepthMap's zero scale gives whatever the quad's z.
// Leaves the target bound and depth testing as the world draws it.
//
// PORT: not C3D_RenderTargetClear(). That is a GX memory fill per target, and
// the seven tiny fills a frame (64x64 faces, the 128x128 sphere) queued back to
// back stalled libctru's GX queue on the 3DS for good on the first world frame
// (TASKS.md #307): the queue counts a command done per completion interrupt,
// and fills that short finish within a few microseconds of each other. Azahar
// delivers every interrupt and never showed it. Drawn, the clear is inside the
// command list in order with the draws, and the queue has nothing to count.
static void envClear(EnvMap* e, C3D_RenderTarget* t, bool depth) {
	C3D_FrameDrawOn(t);
	C3D_BindProgram(&e->prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 3);
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, e->quad, sizeof(EnvVert), 2, 0x10);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(depth, GPU_ALWAYS, depth ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
	C3D_DepthMap(true, 0.0f, 0.0f);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, 0);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	// citro3d's own (C3D_Init's), which nothing else in the port changes.
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

typedef struct {
	const World* w;
	float ambient, gain, invGamma;
} WorldEnv;

static u32 drawWorldFace(void* ctx, const C3D_Mtx* vp, const float eye[3],
		const float fwd[3]) {
	const WorldEnv* we = (const WorldEnv*)ctx;
	return world_draw_env(we->w, vp, eye, fwd, we->ambient, we->gain, we->invGamma);
}

void envmap_render(EnvMap* e, const World* w, const float camEye[3],
		const float envEye[3], float ambient, float gain, float invGamma) {
	if (!w)
		return;
	WorldEnv we = { w, ambient, gain, invGamma };
	envmap_render_with(e, drawWorldFace, &we, camEye, envEye, false);
}

void envmap_render_with(EnvMap* e, EnvMapDraw draw, void* ctx, const float camEye[3],
		const float envEye[3], bool turned) {
	if (!e || !draw)
		return;
	// Faces drawn the other way round are no use to this capture.
	if (turned != e->turned) {
		envmap_reset(e);
		e->turned = turned;
	}
	// UpdateEnvironmentMapCameras: the env camera looks from the world camera
	// at the env eye, world z up -- MTXLookAt's rows are its right, up and
	// back. A camera straight overhead keeps last frame's.
	float (*ax)[3] = e->ax;
	float f[3] = { envEye[0] - camEye[0], envEye[1] - camEye[1], envEye[2] - camEye[2] };
	const float zUp[3] = { 0.0f, 0.0f, 1.0f };
	float rt[3], up[3];
	if (norm3(f)) {
		cross3(rt, f, zUp);
		if (norm3(rt)) {
			cross3(up, rt, f);
			for (int i = 0; i < 3; i++) {
				ax[0][i] = rt[i];
				ax[1][i] = up[i];
				ax[2][i] = -f[i];
			}
		}
	}

	C3D_Mtx proj;
	Mtx_Persp(&proj, C3D_AngleFromDegrees(90.0f), 1.0f, ENV_NEAR, ENV_FAR, false);
	e->tris = 0;
	e->faces = 0;
	for (int k = 0; k < ENV_FACES; k++) {
		const bool due = e->mode == 2 ? true
			: e->mode == 1 ? (e->frame % ENV_FACES) == (u32)k
			: e->mode == 3 ? kSchedule3[e->frame % 3][k] != 0
			: kSchedule0[e->frame & 3][k] != 0;
		// A face never drawn yet goes now, whatever the schedule says, so
		// the first frame's sphere is whole.
		if (!due && e->faceDrawn[k])
			continue;
		if (e->skip & (1u << k)) {
			if (!e->faceDrawn[k]) {
				envClear(e, e->faceTarget[k], true);
				e->faceDrawn[k] = true;
			}
			continue;
		}
		// PORT (#312): SPEED2.EXE's frontend car reads the live cube
		// [0x870858] (0x5bcad0 when GameFlow is not RACING), its faces drawn
		// with these same eViews 0x10-0x15 (0x5ba2e1: +X 0x11, -X 0x13, +Y
		// 0x14, -Y 0x15, +Z 0x10, -Z 0x12, which is this frame's x, y and z).
		// car_fixed_envmap's vs_1_1 (file 0x43b264) looks it up at R times
		// WorldView, though, and the view is eCreateLookAtMatrix's: x right,
		// y down, z forward. So the PC's car sees the faces half a turn about
		// the right axis -- y and z negated -- and that turn is what puts the
		// lit platform in its windshield and an orange streak on its hood.
		// `turned` draws each face looking where that lookup lands; the fold
		// and the car's read stay the GameCube's. The race's cube [0x87088c]
		// is six static textures (XPOS..ZNEG, 0x5b99c0), not this capture,
		// and the world keeps the GameCube's way.
		const float s = turned ? -1.0f : 1.0f;
		float F[3], U[3];
		for (int i = 0; i < 3; i++) {
			F[i] = kFaceF[k][0] * ax[0][i] + s * (kFaceF[k][1] * ax[1][i] + kFaceF[k][2] * ax[2][i]);
			U[i] = kFaceU[k][0] * ax[0][i] + s * (kFaceU[k][1] * ax[1][i] + kFaceU[k][2] * ax[2][i]);
		}
		C3D_Mtx view, vp;
		Mtx_LookAt(&view, FVec3_New(envEye[0], envEye[1], envEye[2]),
			FVec3_New(envEye[0] + F[0], envEye[1] + F[1], envEye[2] + F[2]),
			FVec3_New(U[0], U[1], U[2]), false);
		Mtx_Multiply(&vp, &proj, &view);
		// Black, as eInitEnvMap's capture clears: the sky is not drawn in.
		envClear(e, e->faceTarget[k], true);
		gpuwatch_trace(kTraceClear[k]);
		const u64 tk0 = svcGetSystemTick();
		const u32 ft = draw(ctx, &vp, envEye, F);
		e->faceTicks[k] = svcGetSystemTick() - tk0;
		e->faceTris[k] = ft;
		e->tris += ft;
		gpuwatch_trace(kTraceDraw[k]);
		e->faceDrawn[k] = true;
		e->faces++;
		if (s_kick)
			s_kick();
	}
	e->frame++;
	// Each face's last triangles and CPU time, every 256 frames (#432).
	if (e->log && (e->frame & 255u) == 0) {
		char b[200];
		int n = snprintf(b, sizeof(b), "env faces (tris/ms):");
		for (int k = 0; k < ENV_FACES && n < (int)sizeof(b); k++)
			n += snprintf(b + n, sizeof(b) - n, " %d %lu/%.2f", k, (unsigned long)e->faceTris[k],
				(double)e->faceTicks[k] * 1000.0 / (double)SYSCLOCK_ARM11);
		e->log(b);
	}

	// genSphereMap, over the black clear. envClear() has the program and the
	// attributes up: v0 the disc position, v1 s, t and the weight.
	envClear(e, e->sphereTarget, false);
	gpuwatch_trace("env sphere: clear");
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	for (int k = 0; k < ENV_FACES; k++) {
		if (!e->faceDrawn[k] || !e->nIdx[k])
			continue;
		C3D_TexBind(0, &e->face[k]);
		C3D_BufInfo* bi = C3D_GetBufInfo();
		BufInfo_Init(bi);
		BufInfo_Add(bi, e->verts[k], sizeof(EnvVert), 2, 0x10);
		C3D_DrawElements(GPU_TRIANGLES, e->nIdx[k], C3D_UNSIGNED_SHORT, e->idx[k]);
	}
	gpuwatch_trace("env sphere: the faces folded in");
	if (s_kick)
		s_kick();
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);

	model3d_set_car_env(&e->sphere, ax[0], ax[1], ax[2]);
}

void envmap_draw_debug(const EnvMap* e) {
	if (!e)
		return;
	// The faces 0-2 over 3-5 down the left, the sphere map to their right,
	// each with t = 1 at its top.
	static const Tex3DS_SubTexture face = { ENV_FACE_SZ, ENV_FACE_SZ, 0.0f, 1.0f, 1.0f, 0.0f };
	static const Tex3DS_SubTexture sph = { ENV_SPHERE, ENV_SPHERE, 0.0f, 1.0f, 1.0f, 0.0f };
	for (int k = 0; k < ENV_FACES; k++) {
		const C2D_Image img = { (C3D_Tex*)&e->face[k], &face };
		C2D_DrawImageAt(img, 2.0f + (float)(k % 3) * 66.0f, 2.0f + (float)(k / 3) * 66.0f,
			0.9f, NULL, 1.0f, 1.0f);
	}
	const C2D_Image img = { (C3D_Tex*)&e->sphere, &sph };
	C2D_DrawImageAt(img, 200.0f, 2.0f, 0.9f, NULL, 0.9f, 0.9f);
}

void envmap_stats(const EnvMap* e, u32* outTris, int* outFaces) {
	if (outTris)
		*outTris = e ? e->tris : 0;
	if (outFaces)
		*outFaces = e ? e->faces : 0;
}
