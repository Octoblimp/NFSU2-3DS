#pragma once
#include <3ds.h>
#include <citro2d.h>

// The in-game photo shoot's data (TASKS.md #268 B): the camera setups, the
// magazines and their mastheads, out of retail's two FRONTEND files.
//
// MAGAZINES_SHOWCASE.BIN -- SCMagazineDatabase
// --------------------------------------------
// One chunk 0x00030231 whose payload is the database as the game holds it in
// memory (LoadShowcaseMagazineDatabase, SpeedPS.elf 0x800E9028, loads it
// whole and points the database at it): three counts, then three fixed
// arrays sized for 64 car setups, 32 render setups and 32 magazines, of
// which the shipped file fills 42, 1 and 14.
//
//   +0x000  u32 nCar, nRender, nMag
//   +0x00C  car setup[64], 44 bytes: hash, u32 0, then the nine floats
//           SetShowcaseCameraParams (0x800760E4) takes -- yaw and elevation
//           in degrees, distance, the pivot's bias off the car (x along it,
//           y across it, z up the ground normal), the field of view in
//           degrees, and two the camera keeps at +0xB4 / +0xB8 (zero in every
//           shipped setup).
//   +0xB0C  render setup[32], 12 bytes: hash and two words.
//   +0xC8C  magazine[32], 56 bytes: hash, u32 0, nMast, nExtra, nCar,
//           nRender, mast[2], extra[1], car[3], render[2].
//
// MAGAZINES_STREAMING_LOW.bin -- the mastheads
// --------------------------------------------
// A streaming texture pack: 0xB3300000 holding a 0xB3310000 whose
// 0x33310003 record lists every texture as {hash, file offset, compressed
// size, size, 0x100, 0}. Each one is its own JDLZ block in the file; it
// unpacks to the texture's pixels (a 512x512 DXT3 masthead) followed by its
// 156-byte TextureInfo, which names the texture (+12), gives its width and
// height (+68, +70) and its format's FourCC (+144). MagazineTextureHandler
// loads the two a selection names, the masthead and the extra.
#define MAGDB_CARS    64
#define MAGDB_RENDERS 32
#define MAGDB_MAGS    32

typedef struct {
	u32 hash;
	float yaw, elev, dist;  // degrees, degrees, metres
	float bias[3];          // along the car, across it, up the ground normal
	float fov;              // degrees
	float p8, p9;           // the camera's +0xB4 and +0xB8 (the depth of field)
} MagCarSetup;

typedef struct {
	u32 hash;
	u32 w[2];
} MagRenderSetup;

typedef struct {
	u32 hash;
	u32 nMast, nExtra, nCar, nRender;
	u32 mast[2], extra[1], car[3], render[2];
} MagSetup;

typedef struct {
	int nCar, nRender, nMag;
	MagCarSetup car[MAGDB_CARS];
	MagRenderSetup render[MAGDB_RENDERS];
	MagSetup mag[MAGDB_MAGS];
} MagDb;

// SCMagazineSelection: what GenerateMagazineSelection picked.
typedef struct {
	u32 mast, extra, car;
	int carIdx;
	u32 render;
} MagSelection;

// The whole file into `db`. False (and `db` empty) when it is missing or not
// the database.
bool magdb_load(MagDb* db, const char* path);

// GetMagazineSetupByHash / GetCarSetupByHash: NULL when there is none.
const MagSetup* magdb_find_magazine(const MagDb* db, u32 hash);
const MagCarSetup* magdb_find_car(const MagDb* db, u32 hash);

// SCMagazineSetup::GenerateMagazineSelection (SpeedPS.elf 0x8018558C): a
// masthead and an extra among the magazine's own (bRandom only over two or
// more), a car setup and a render setup (bRandom over any). `rnd(n)` is the
// game's bRandom.
void magdb_generate_selection(const MagSetup* m, MagSelection* sel, u32 (*rnd)(u32 n));

// SCMagazineSetup::CycleCarSetups (0x8018565C): the next of the magazine's
// car setups, round to the first after the last.
void magdb_cycle_car_setups(const MagSetup* m, MagSelection* sel);

// MAGAZINES_FRONTEND.BIN -- FEMagazineDatabase
// ---------------------------------------------
// UIMagazineReward's (SpeedPS.elf 0x801868FC), the frontend cover a type 3
// or 4 showcase leads to. One chunk 0x00030230 laid out like the in-game
// one, held whole (LoadFrontendMagazineDatabase, 0x80186DF4): 37 car setups,
// 3 render setups and 16 magazines in the shipped file.
//
//   +0x000  u32 nCar, nRender, nMag
//   +0x00C  car setup[64], 56 bytes (FECarSetup): hash, flags (bits 1, 2, 4
//           and 8 open the car parts at animation locations 0, 2, 3 and 1:
//           FEDoCarPartAnimNow), then twelve floats CarViewer::
//           SetCameraParams (0x800FEA4C) takes -- the car's turn and its
//           front wheels' angle in degrees (CarOrbiter::
//           SetZRotationAndTireAngle), then SelectCarCameraMover's view: the
//           field of view in degrees, the target and the eye in the
//           garage's units, the depth of field's two (the camera's +0xB4 /
//           +0xB8) and the roll about the view axis in degrees
//           (SetLookAtAxisRoll).
//   +0xE0C  render setup[32], 24 bytes (FERenderSetup): hash, the shaper
//           lights' hash (CarViewer::SetShaperLights) and the env map's name,
//           loaded as FRONTEND/ENVMAPS/<name>.BIN.
//   +0x110C magazine[32], 0x68 bytes (FEMagazineSetup): hash, a word, the six
//           lists' counts, then the lists: back[3], mast[2], chick[5],
//           extra[3], car[3], render[2].
//
// The textures are MAGAZINES_STREAMING_HIGH.bin's (MagazineTextureHandler::
// Init(0, 0) takes pack 0 of r13-0x6F18's two, HIGH; the in-game shoot's is
// LOW): every one 512x512 with the cover in its top-left ~331x429. The backs
// are DXT1 but for SWT_BG_01..03, RSU_BG_03, GTI_BG_01 and FOTF_BG_01 (DXT3),
// the rest DXT3; STF_ANNA_01, STF_JAMIE_01 and SS_BG_01..03 are D3DFMT_P8.
#define FEMAG_CARS    64
#define FEMAG_RENDERS 32
#define FEMAG_MAGS    32

typedef struct {
	u32 hash;
	u32 flags;
	float zRot, tireAngle;  // degrees
	float fov;              // degrees
	float target[3], eye[3];
	float dof[2];
	float roll;             // degrees
} FeCarSetup;

typedef struct {
	u32 hash;
	u32 shaper;
	char envmap[17];
} FeRenderSetup;

typedef struct {
	u32 hash;
	u32 nBack, nMast, nChick, nExtra, nCar, nRender;
	u32 back[3], mast[2], chick[5], extra[3], car[3], render[2];
} FeMagSetup;

typedef struct {
	int nCar, nRender, nMag;
	FeCarSetup car[FEMAG_CARS];
	FeRenderSetup render[FEMAG_RENDERS];
	FeMagSetup mag[FEMAG_MAGS];
} FeMagDb;

// FEMagazineSelection: one of each list.
typedef struct {
	u32 back, mast, chick, extra, car, render;
} FeMagSelection;

bool femag_load(FeMagDb* db, const char* path);
const FeMagSetup* femag_find_magazine(const FeMagDb* db, u32 hash);
const FeCarSetup* femag_find_car(const FeMagDb* db, u32 hash);
const FeRenderSetup* femag_find_render(const FeMagDb* db, u32 hash);

// FEMagazineSetup::GenerateMagazineSelection (0x80185470): bRandom over a
// texture list only when it has two or more, over the car and render lists
// whenever they have any.
void femag_generate_selection(const FeMagSetup* m, FeMagSelection* sel, u32 (*rnd)(u32 n));

typedef struct {
	u32 hash;  // 0 when nothing is loaded
	C3D_Tex tex;
	Tex3DS_SubTexture subtex;
	C2D_Image image;
	bool valid;
} MagTex;

void magtex_free(MagTex* t);

// One texture out of the streaming pack at `path`, decoded and box-filtered
// down by halves until neither side is over `maxDim`. False, with `t` left
// empty, when the pack or the texture is missing or will not decode.
bool magtex_load(MagTex* t, const char* path, u32 hash, int maxDim);

// The same out of raw B, G, R, A pixels, rows top down (a kept cover's), `w`
// and `h` powers of two. False, with `t` left empty, when it will not decode.
bool magtex_from_bgra(MagTex* t, const u8* bgra, int w, int h, u32 hash, int maxDim);
