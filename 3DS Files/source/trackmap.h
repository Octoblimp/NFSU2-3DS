#pragma once
#include <3ds.h>
#include <citro2d.h>

// Per-track route map for UI_QRTrackSelect (TASKS.md #115) -- the panel that
// shows the Bayview city map with the selected track's route drawn on it.
//
// What the engine does
// --------------------
// The map is not authored into the .fng. UI_QRTrackSelect.fng's Image widget
// with object hash 0x90FE80F3 authors `Track4000_map.tga` as a placeholder,
// and the engine re-textures that one widget every time the selection changes.
// Recovered from SPEED2.EXE:
//
//   * 0x4E2F30 (the track-select screen constructor) looks the widget up twice
//     with FindWidget(package, 0x90FE80F3) and stores it on the map-panel
//     object it constructs at this+0x118 (vtable 0x78F8C8).
//   * 0x4B53C0 (that object's per-selection load, called from 0x4CE754) reads
//     the current track id from *(short*)([0x863448] + 0x18) and then:
//       sprintf(buf, 0x20, "TRACKS\TRACKMAP%d.bin", trackId)   // str @ 0x791864
//       this+0x58 = StringHash(sprintf("TRACKMAP%d", trackId))  // str @ 0x791858,
//                                                               // hash 0x43DB50
//       size  = GetFileSize(buf)                                 // 0x57D900
//       alloc = MemAlloc(size, "TrackMapMemory", 0)               // 0x5E8670,
//                                                               // str @ 0x791848
//       AsyncLoad(buf, 5, 0, 0, size) -> this+0x4C               // 0x57CEF0
//     with completion callback 0x4B53A0, which sets this+0x50 = 1 and calls
//     0x4901D0(this+0x58, 0, 0) -- the by-hash texture lookup across every
//     loaded pack.
//   * 0x4E04AA is the site that proves the pairing outright: FindWidget(pkg,
//     0x90FE80F3) immediately followed by SetTextureHash(widget,
//     StringHash("TRACKMAP%d", [0x89E7A0])). SetTextureHash is 0x50C990
//     (widget+0x24 = hash; widget+0x1C |= 0x400000).
//
// So the whole mechanism is: load TRACKS/TRACKMAP<id>.BIN, which is a texture
// pack holding exactly one texture named TRACKMAP<id>, and point widget
// 0x90FE80F3 at it. Verified against the shipped data:
// bStringHash("TRACKMAP4011") == resource_texture_hash("TrackMap4011.tga")
// == 0xF7600958, which is exactly the hash stored inside TRACKMAP4011.BIN.
//
// What the files are
// ------------------
// PC Files/TRACKS/TRACKMAP*.BIN, 129 files, every one exactly 350272 bytes:
// a single 0xB3300000 container named
// `Location4\RegionA\Pipeline\PC\TrackMap<id>.tpk` holding one 512x512 DXT3
// texture `TRACKMAP<id>` -- the same frontend/global TPK variant texpack.c
// already parses, so no new container code is needed.
//
// One trap in them: the alpha channel is empty. Every DXT3 block's alpha half
// is zero (checked over all 129 files, not just one), even though the
// 0x33310005 format record's +0x08 is 1 exactly as it is for a genuinely
// translucent DXT3 like U2_MENU_BOX, and even though the widget's own authored
// Color track is plain opaque white. Decoded literally the texture is
// invisible and the panel draws blank -- which is what the first Azahar pass
// of this loader showed. texpack_decode_entry()'s `opaqueIfAlphaEmpty` is the
// narrow, evidence-checked fix: it promotes alpha to 255 only when every texel
// decoded to 0, which by construction cannot lose an image.
//
// Downscaling
// -----------
// Decoded at native size a map is 512*512*4 == 1MiB of linear memory, and one
// is resident for as long as the screen is up. The widget's authored size is
// 261 canvas units, which at UI_QRTrackSelect's layout scale is ~163 screen
// pixels, so a 256x256 copy is still above the resolution anything is drawn
// at. TRACKMAP_MAX_DIM takes that halving up front rather than holding it in
// reserve: 256KiB resident and a 256KiB transient tiling buffer instead of
// 1MiB + 1MiB, on a machine where the frontend already holds a 6MB pack open
// at load time.
#define TRACKMAP_MAX_DIM 256

// The free-roam minimap (minimap.c) is the other user of this loader and wants
// the opposite trade: it shows a 0.2 x 0.2 slice of the art blown up to fill
// the bottom screen, so halving the source would cost real detail rather than
// resolution nothing samples. It loads at native size, which is 1MiB resident
// -- affordable there because the world, not the frontend, is what is up.
#define TRACKMAP_FULL_DIM 512

// Exactly one map is resident at a time -- trackmap_load() replaces whatever
// was there. That mirrors the engine, whose map-panel object owns a single
// TrackMapMemory allocation it frees and re-fills per selection, and it is why
// this does not go through TexCache (which never evicts).
typedef struct {
	int trackId; // -1 when nothing is loaded
	C3D_Tex tex;
	Tex3DS_SubTexture subtex;
	C2D_Image image;
	bool valid;
} TrackMap;

void trackmap_init(TrackMap* m);
void trackmap_free(TrackMap* m);

// bStringHash of "TRACKMAP%d" % trackId -- SPEED2.EXE 0x50B990 (sprintf) +
// 0x43DB50 (the hash), format string @ 0x791858.
u32 trackmap_texture_hash(int trackId);

// "TRACKS/TRACKMAP%d.BIN" -- SPEED2.EXE's own "TRACKS\TRACKMAP%d.bin" @
// 0x791864, with the separator and case the shipped files actually use on a
// case-sensitive filesystem (the SD card's are uppercase .BIN).
void trackmap_rel_path(int trackId, char* out, size_t outSize);

// Loads TRACKS/TRACKMAP<trackId>.BIN from `gameDirPath` (already-built
// absolute path to the file) and uploads its single texture. A no-op returning
// true when `trackId` is already the resident one. On failure the previous map
// is freed and `m->valid` is left false, so a missing file draws nothing
// rather than the wrong track's route.
//
// `maxDim` is texpack_decode_entry()'s: the decoded image is box-filtered down
// by halves until neither axis exceeds it. Pass TRACKMAP_MAX_DIM for the
// track-select panel, TRACKMAP_FULL_DIM for the minimap.
bool trackmap_load(TrackMap* m, const char* filePath, int trackId, int maxDim);
