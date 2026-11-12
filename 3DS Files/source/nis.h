// The in-game NIS ("non-interactive sequence") player, TASKS.md #343.
//
// Retail plays a scene through CAnimPlayer: the scene bundle
// (NIS/Scene_<name>_BundleB.bun) holds its header (chunk 0x37030: the
// bStringHash, the name, the scene type and the length in 30 Hz frames) and
// its EAGL anim banks (0x00E34010), and its camera is a DragCameraGroup of the
// same hash in the in-game bundle's NIS camera set (chunk 0x8003B201,
// LoadCameraSet 0x80069660), played by IceCameraMover against the scene's
// parameter, time / length (DragCameraManager::GetParameter).
//
// What is ported: the header, the camera group's first track and
// IceCameraMover's evaluation of it -- key choice (GetKeyNumber), the eye,
// look and lens cubics with GetSlope's tangents (SetDesired 0x80073FC4,
// MakeCoeffs 0x801A10D8, GetEye / GetLook / GetFOV 0x80074DD8..), and the
// letterbox cubic (+0x50, x 0.00125, eDisplayLetterBoxes 0x801DA8A4).
// PORT: keys whose ends are shared with a neighbour (KeysShared's blended
// slope) and blend-in keys (+1, the mover's carried-over cubic) are taken as
// unshared hard cuts, Car / Hybrid spaces as World (Scene goes through
// nis_set_scene_matrix, #354); the scenes the
// port plays (CareerNis1) have only World keys, none shared, none blended.
#ifndef NIS_H
#define NIS_H

#include <stdbool.h>
#include <stdint.h>

#define NIS_MAX_KEYS 16

typedef struct {
	uint8_t flags[4];     // +0 smooth, +1 blend, +2 eye / fov cubic, +3 look cubic
	uint8_t space[2];     // +4 eye, +5 look (0 Car, 1 World, 2 Hybrid, 3 Scene)
	float t;              // +0xC the start parameter
	float tan[2];         // +0x10
	float eye[2][3];      // +0x18
	float look[2][3];     // +0x30
	float lens[2];        // +0x48
	float focal[2];       // +0x68 the focal distance, metres (#326)
	uint8_t aperture[2];  // +0x70 the aperture number, 37 and up shut (#326)
	uint8_t letterbox[2]; // +0x72
} NisCamKey;

typedef struct {
	bool loaded;
	uint32_t hash;
	char name[20];
	int type;             // header +0x18 (3 shop enter, 4 career start / shop exit)
	int frames;           // header +0x28
	float length;         // frames / 30 (DAT_80319220)
	int goFrames;         // header +0x2C: a race start's GO (#244)
	int countdownFrames;  // header +0x30: the StartCountdown marker (#244)
	int keyCount;
	NisCamKey key[NIS_MAX_KEYS];
	int track;            // the camera group's track played
	float scene[4][3];    // Scene space (3) -> world, rows as axes, row 3 the origin
} NisScene;

typedef struct {
	float eye[3], look[3];
	float fovDeg;         // camera +0xC4 in degrees
	float letterbox;      // each bar's share of the view's height
	float focal;          // the focal distance cubic, metres (#326)
	float aperture;       // the aperture number cubic (#326)
	int key;
} NisView;

uint32_t nis_hash(const char* s);   // bStringHash
// The header out of `sceneBun` and the camera group of the same hash out of
// `camBun`. False, with `log` told why, when either is missing.
bool nis_load(NisScene* s, const char* sceneBun, const char* camBun, void (*log)(const char*));
// The same with the header out of `chunk` (a .crowd's scene chunk, crowd.h)
// and camera track `track` (the End NIS candidate's +4, TASKS.md #354).
bool nis_load_mem(NisScene* s, const void* chunk, uint32_t bytes, const char* camBun, int track,
	void (*log)(const char*));
// Where Scene-space (3) keys stand in the world (FindRaceAnimMatricies);
// identity after a load.
void nis_set_scene_matrix(NisScene* s, const float m[4][3]);
// A top-level chunk `want` of the bundle at `path`, its body only (malloc;
// the caller frees it), NULL when missing.
uint8_t* nis_read_chunk(const char* path, uint32_t want, uint32_t* size);
// The camera at scene time `t` seconds.
void nis_view(const NisScene* s, float t, NisView* v);
// The depth of field the view's lens puts on the camera (#326):
// IceCameraMover::Update's camera +0xB4 (the middle of the sharp span, metres)
// and +0xB8 (its length). False, both 0, when the aperture is shut (37 and
// up), as on all but a few of the scenes' keys.
bool nis_dof(const NisView* v, float* focus, float* span);

#endif
