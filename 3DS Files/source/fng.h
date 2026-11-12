#pragma once
#include <3ds.h>

// Runtime widget-tree parser for a decompressed real `.fng`/FEPackage screen
// file (e.g. UI_Main.fng, pulled out of PC Files/FRONTEND/FrontB.lzc via
// jdlz.c -- see tools/fng_extract.py). C port of tools/fng_parse.py; see that
// file's docstring and docs/formats/frontend-menu-re.md for the full on-disk
// format writeup this implements. Only the fields needed to draw a first real
// render pass are extracted (Ot type, OP resourceIndex+GUID, SH string hash,
// SA position/size floats, PA parent GUID, Res\xCC/RsRq resource-record
// table for resolving resourceIndex to a real texture hash).
//
// Widget hierarchy IS modeled (session 29, TASKS.md #50): a background
// Ghidra decompile of the real render path (`Render__7FEngine` @ 80147540 ->
// per-Group `RenderGroup__7FEngineP7FEGroupR9FEMatrix4T2Us` @ 80147728 ->
// leaf `RenderObject__7FEngine` @ 801478f8) confirmed the engine composes a
// widget's final render position by walking its `PA`-established parent
// chain recursively (via each Group's real `FEMinList` child list at
// FEObject+0x60, not a single level) and matrix-multiplying every ancestor's
// local transform in. This parser only ports the *position* half of that (a
// flat 2D sum, no rotation/pivot matrix -- overkill for the flat menu
// screens seen so far): `guid` is the widget's own OP-tag GUID (what a
// child's `parentGuid` matches against), `parentIndex` is resolved by
// `fng_parse()` after the whole screen is walked (a widget can reference a
// `PA` GUID declared later in the file) into a direct index into
// `FngScreen.widgets`, or -1 if unset/unresolved. Callers should sum
// posX/posY up the `parentIndex` chain before mapping to screen space.
//
// Position/size come from the `SA` tag, NOT `OP`'s first two fields (those
// are a GUID + a field shared with the separate `Oh` tag, confirmed by
// decompile -- see TASKS.md #50). `SA`'s payload is copied verbatim into the
// live FEObject's
// per-object "track data" buffer (`+0x2c`), and `GetDataOffset__8FEObject18FEKeyTrack_Indices`
// (Speed.elf @ 80138b70) gives the byte offset of each track within that
// buffer: index 2 (Position, Vector3) -> +0x1c, index 3 (Rotation quaternion)
// -> +0x28, index 4 (Size, Vector3) -> +0x38. Verified against real
// UI_Main.fng bytes: rotation reads back as the identity quaternion
// (0,0,0,1) and size as sane on-screen pixel dimensions (e.g. 385x32,
// 118x59, 32x32) for every widget checked -- not a coincidental byte
// pattern.
//
// Which of Position's three components is the on-screen vertical axis was
// previously read wrong (+0x24 was taken as vertical and +0x20 as an unused
// "depth"). They are the other way round: +0x20 is Y and +0x24 is the
// z-order/depth the engine sorts draws by. Established from real shipped data
// plus the reference PC title screenshot, not guessed:
//
//  - Loading.fng parents a 299.5x36 text box at (7.92, -0.33, 15) and a 36x36
//    icon at (175.70, -0.3125, 40) to the same group. Under "+0x20 is Y" they
//    share a baseline (-0.33 vs -0.31) and 15-vs-40 is a sensible
//    behind/in-front ordering; under the old reading the icon would sit 25
//    units below the box for no reason.
//  - Full-canvas 640x480 backgrounds carry the largest +0x24 on their screen
//    (Loading.fng's PC_load.tga = 130, UG_LS_Splash.fng's = 1000) and +0x20 = 0
//    -- i.e. "centered, furthest back", which is what a backdrop is. Read the
//    old way a backdrop would be flung 130/1000 units off-center vertically.
//  - UG_LS_Splash.fng (the real title screen) vs. the reference PC screenshot:
//    its "(c) 2004 Electronic Arts Inc." string has +0x20 = 194, and the
//    screenshot puts that line at canvas y ~ +187; its prompt's parent group
//    has +0x20 = -51 and the screenshot puts "Press Enter Key" at y ~ -50.
//    Its "Licensed by Some Company" placeholder has +0x20 = 463, past the
//    240 half-height, and is correspondingly absent from the screenshot.
//
// So the space is the 640x480 canvas centered on the origin with +Y DOWN, and
// callers must sort by posDepth (descending == furthest first) to draw
// correctly rather than relying on file order.

typedef enum {
	FNG_TYPE_IMAGE = 1,
	FNG_TYPE_STRING = 2,
	FNG_TYPE_LISTBOX = 4,
	FNG_TYPE_GROUP = 5,
	FNG_TYPE_CODELISTBOX = 6,
} FngWidgetType;

typedef struct {
	int type; // FngWidgetType, or 0/unset if this ObjD had no Ot tag
	bool hasPos;
	// SA offset +0x1c/+0x20/+0x24. posX/posY are the on-screen axes (centered
	// 640x480 canvas, +Y down); posDepth is z-order, not a screen axis. See
	// the comment block above for how this was pinned down.
	float posX, posY, posDepth;
	// SA offset +0x00..+0x10: the object's Color track, four u32 channels in
	// B,G,R,A order, each 0-255 -- an FEColor laid out verbatim. Track index
	// 0 in the same GetDataOffset__8FEObject18FEKeyTrack_Indices layout the
	// position/size offsets come from, so it sits at the start of the buffer.
	// The channel order is the engine's, not a guess: FEColor's u32
	// constructor (Speed.elf 0x801424d4) stores c & 0xff at +0, c >> 8 at +4,
	// c >> 16 at +8 and c >> 24 at +0xc, and its operator unsigned long
	// (0x801424fc) packs them back the same way, so for the engine's 0xAARRGGBB
	// colours +0 is blue and +8 is red. This was once read R,G,B,A, which
	// swapped red and blue on every coloured widget. The shipped data agrees:
	// UG_LS_Splash.fng's prompt string and Loading.fng's loading dots share one
	// accent (202,244,138), the yellow-green of "Press START" in
	// gamecube_title_screen.jpg (read the old way it was a mint green), and
	// HUD_SingleRace.fng's nitrous fill is (0,90,125), the nitrous blue,
	// where the old reading made it orange. Most widgets are
	// (255,255,255,255); UG_LS_Splash's band behind the prompt is (0,0,0) at
	// alpha 60 (a translucent black scrim, matching the reference screenshot),
	// and the gradient bars flanking the copyright line are white at alpha
	// 150.
	bool hasColor;
	u8 colorR, colorG, colorB, colorA;
	// SA offset +0x28, the Rotation track's quaternion (x, y, z, w). This is
	// how one piece of corner or arrow art serves every slot it is drawn in:
	// UI_QRModeOptions.fng's LEFT_ARROW_n slots carry (0, 0, 1, ~0), a 180
	// degree turn about Z, to get a left-pointing arrow out of the single
	// right-pointing sprite AddRow binds to both slots. Turns about X and Y
	// are just as common -- across every shipped .fng, 443 Images carry
	// (-1, 0, 0, 0) and 271 carry (0, 1, 0, 0), which seen face-on are a
	// vertical and a horizontal mirror; that is how UI_EngageRaceDialog makes
	// its four rounded corners from one U2_Menu_RoundedCorner.
	//
	// The FE is drawn face-on, so all a 2D draw needs is the rotation matrix's
	// screen-plane 2x2 (rotM, row-major: x' = m00*x + m01*y, y' = m10*x +
	// m11*y, +Y down as everywhere else here). rotAngle is its Z part alone,
	// kept for the callers that only ever see a Z turn.
	bool hasRot;
	float rotAngle; // radians, 2*atan2(z, w)
	float rotM[4];  // m00, m01, m10, m11

	bool hasSize;
	float sizeW, sizeH; // SA offset +0x38/+0x3c, pixel dimensions
	bool hasResourceRef;
	u32 resourceHash; // resolved via Res\xCC/RsRq using OP's resourceIndex field
	bool hasStringHash;
	u32 stringHash;
	bool hasGuid;
	u32 guid; // OP tag payload offset 0 -> FEObject+0xc; what a child's PA/parentGuid matches against
	// OP tag payload offset +4 -> FEObject+0x10, the object's *name* hash (the
	// same field the separate Oh tag writes). This is the handle the engine's
	// screen code addresses individual authored widgets by at runtime: the
	// real UI_StartCareer builder (SPEED2.EXE 0x4AF110) does
	// `push <labelHash>; call 0x4FFA80; push eax; push 0x42ADB44C; push <screen>;
	// call 0x537B80` -- i.e. "look up this language string, then set it as the
	// text of the widget whose name hash is 0x42ADB44C". 0x42ADB44C is exactly
	// UI_StartCareer.fng's guid=904621 String, whose own authored St
	// placeholder reads "Career" while its SH resolves to "Options" (the
	// screen was cloned from UI_OptionsMain.fng and retitled at runtime) --
	// which is what makes this field, not the SH, the truth for a screen title.
	bool hasObjHash;
	u32 objHash;
	bool hasFlags;
	// OP tag payload offset +8 -> FEObject+0x14, a per-object state/flags word.
	// Bit 0x80000000 empirically confirmed (session 2026-08-31, task #83) as a
	// real "inactive by default" marker: across UG_LS_Splash.fng, MC_Background.fng,
	// MC_Main.fng, and UI_Main.fng (every screen parsed so far), exactly one
	// widget anywhere carries this bit -- MC_Main.fng's guid=911062 String
	// widget, whose real (not fabricated) embedded text is the PS2-specific
	// "Checking for memory card (PS2) in MEMORY CARD slot 1..." warning. That
	// message being unconditionally drawn (nothing filtered on `flags` before
	// this task) is what produced the second garbled text line investigated
	// this session -- it's real authored content for a state (PS2 memory-card
	// I/O) this 3DS port never enters, not a rendering bug. Every other widget
	// across all four screens has only 0x4-class top nibbles (0x40000000/
	// 0x50080000/0x40000040/etc) -- never 0xC. Treated as "skip drawing" below.
	//
	// What retail actually does with the bit is narrower: FEngine::RenderGroup
	// (Speed.elf 0x80147728) skips an object with it only while FEngine+0, the
	// execution flag, is clear, and cFEng's constructor (0x8014d4e4) sets
	// execution on, so in play the bit hides nothing by itself; the engine's
	// own invisible bit is 0x1 (cFEng::RenderObject 0x8014d928, set by
	// FEngSetInvisible 0x80150168), which no shipped .fng authors. The skip is
	// kept because on the screens this port shows, the one drawable widget the
	// bit is on is still the PS2 string above -- the other five carriers there
	// are Groups (UI_EngageRaceDialog's highlight frame, UI_EngageMessage
	// Dialog's, four on HUD_SingleRace), and a Group's children are not
	// affected by its own flags in drawMenu().
	u32 flags;
	bool hasParentGuid;
	u32 parentGuid; // PA tag payload, a GUID resolved via FindObjectByGUID
	int parentIndex; // resolved by fng_parse() into an index into FngScreen.widgets, or -1
	// St tag payload: a literal UTF-16LE string embedded directly in this
	// widget's ObjD entry (confirmed present on LS_LangSelect.fng's "French"
	// String widget, whose SH stringHash does not resolve in English.bin --
	// this is the on-disk mechanism for text that can't go through a
	// language-dependent hash table before a language has been chosen).
	// tools/fng_parse.py does not decode this tag either (it falls into that
	// tool's generic hex-only fallback too); this is a plain UTF-16LE ->
	// ASCII decode of real on-disk bytes, not invented behavior.
	bool hasLiteralText;
	char literalText[64];
	// A String widget's OP resourceIndex does NOT point at a texture -- it
	// points at one of the screen's `.ffn` FONT records (kind=2), which is how
	// the shipped data picks a point size per widget. UG_LS_Splash.fng declares
	// both ConduitMdITC_TT14i.ffn and ConduitMdITC_TT21i.ffn and its three
	// Strings pick index 4 (14pt, the copyright line) and index 5 (21pt, the
	// "press START" prompt) -- exactly the size relationship in the reference
	// capture. UI_Main.fng declares only ConduitMdITC_TT21i.ffn, so all eight
	// of its Strings resolve to that single 21pt record and vary their size
	// through SA's sizeW/sizeH scale instead (0.6/0.7/0.9/1.0).
	//
	// Two consequences, both handled here rather than at the call site:
	//   * a font reference is NOT a resourceHash -- setting hasResourceRef for
	//     it made every UI_Main String share one hash at four different SA
	//     sizes, which a since-removed "9-slice chrome" skip in main.c read as
	//     border pieces, so all eight were silently skipped (that is the whole
	//     reason no text drew on the main menu, task #83);
	//   * the point size is recoverable from the record name's own trailing
	//     digit run (ConduitMdITC_TT21i.ffn -> 21), which is real shipped data.
	bool hasFontRef;
	u16 fontPointSize; // parsed from the .ffn record name, 0 if unparseable
	// ffn_name_hash() of that record's name: the key to the game's own font
	// chunk (ffn.h), which is where the metrics really come from.
	u32 fontNameHash;
	// Sj tag: text justification, as a pair of independent offsets the engine
	// adds to the authored origin. cFEngRender::RenderString reads it from
	// FEString+0x6c and hands it to FEngFont::RenderString, which resolves it
	// through two functions that are the entire rule (Speed.elf, decompiled):
	//
	//   CalculateXOffset (0x8014cee4): 0x1 -> -width/2, 0x2 -> -width, else 0
	//   CalculateYOffset (0x8014cf1c): 0x4 -> -height/2, 0x8 -> -height, else 0
	//
	// the -0.5 and 0.0 factors being the constants at 0x8032ac78/7c and
	// 0x8032ac80/84. So bit 0x1 centers horizontally, 0x2 right-aligns, 0x4
	// centers vertically and 0x8 bottom-aligns; with neither bit of a pair set
	// the authored x is the string's left edge and the authored y is the TOP of
	// its line box. This confirms what the shipped screens had already implied
	// -- the splash copyright is Sj=5 at canvas x=0 (must be centered), UI_Main's
	// "Main Menu" is Sj=2 at x=285 hard against the canvas's right edge, its
	// "Option title" is Sj=1 at x=-1, and its "Help" pair is Sj=4 at
	// x=211.7/241.7 reading left-to-right -- and adds the 0x8 case, which no
	// screen this port has parsed happens to use.
	bool hasJustify;
	u32 justify;
	// Sw tag: FEString+0x74 (ReadObjectTags 0x8013bbac), the word-wrap
	// width. FEngFont's line walkers -- GetLineWidth (0x8014ca0c),
	// GetNumWordWrappedLines (0x8014cf54), RenderString (0x8014c1f0) -- end a
	// line once the sum of its glyph advances, in the font's own unscaled
	// units, reaches it, going back to the last space. 0 means no wrap.
	bool hasMaxWidth;
	u32 maxWidth;
	// Sl tag: FEString+0x70 (ReadObjectTags' 0x6c53 arm), extra space between
	// lines in font units. FEngFont::RenderString steps each line down by the
	// font's line height plus this, and a wrapped string's height is that
	// pitch times its line count (0x8014c2c0).
	bool hasLeading;
	s32 leading;
	// Which FEObjData members this object's Scrp scripts animate: one bit per
	// track, at the track's LongOffset in dwords (FNG_TRACK_*). The port runs
	// no scripts, so a set bit says the SA value is only where an animation
	// starts, not what the object shows.
	u32 scriptTrackMask;
	// The INIT script's Color track, when INIT loops and the track is a real
	// animation (fng.c's parse_scrp; fng_loop_color() plays it). 0 keys when
	// there is none. B,G,R,A, as the Kd rows store them.
	u8  loopColorKeys;     // its keys, at FngScreen.colorKeys[loopColorFirst]
	u8  loopColorFirst;
	u8  loopColorAction;   // the track's interp action & 0x7f: 0 clamp, 1 wrap, 2 ping-pong
	u8  loopColorBase[4];  // the Kd base row: the values the keys are deltas from
	u16 loopColorMs;       // the script's length (Sh), which its time wraps at
	u16 loopColorTrackMs;  // the track's length (FI), which a wrapping track spans
	// The object is invisible from the moment its package is up: its INIT
	// script chains to HIDE, and HIDE's Color track sets alpha 0.
	// FEPackageReader::ReadScriptTags (Speed.elf 0x8013c680) stores an "Sc"
	// record as the script's chain (FEScript+0x18), the script run when it
	// ends, and a zero-length INIT ends at once. Only a later script brings
	// such an object back -- from a MsgR message response (hasMsgResp) or
	// from the screen's own code.
	bool initHides;
	// The FOb carries a MsgR chunk: message responses, any of which can run
	// one of the object's scripts (a dialog's FADEIN, say).
	bool hasMsgResp;
} FngWidget;

// One key of a looping INIT Color track: its time in ms, and its B,G,R,A as
// deltas from the track's base row.
typedef struct {
	s32 t;
	s16 d[4];
} FngColorKey;

#define FNG_MAX_COLOR_KEYS 128

// FEObjData member offsets in dwords, as a script track's LongOffset names
// them: FEColor at +0x0, pivot +0x10, position +0x1c, rotation +0x28, size
// +0x38 (the same layout SA is copied over).
#define FNG_TRACK_COLOR (1u << 0x0)
#define FNG_TRACK_PIVOT (1u << 0x4)
#define FNG_TRACK_POS   (1u << 0x7)
#define FNG_TRACK_ROT   (1u << 0xa)
#define FNG_TRACK_SIZE  (1u << 0xe)

#define FNG_MAX_WIDGETS 512

typedef struct {
	FngWidget widgets[FNG_MAX_WIDGETS];
	int widgetCount;
	FngColorKey colorKeys[FNG_MAX_COLOR_KEYS];
	int colorKeyCount;
} FngScreen;

// Parses a fully-decompressed .fng buffer (starting with the "FEn\xE7" outer
// magic) into a flat widget list. Returns false if the magic doesn't match.
bool fng_parse(const u8* buf, size_t len, FngScreen* out);

// `w`'s looping INIT Color track at `ms` since the screen went up, into
// out[] as B,G,R,A 0..255. False when the widget has none.
bool fng_loop_color(const FngScreen* s, const FngWidget* w, u32 ms, u8 out[4]);
