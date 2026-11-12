#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "display.h"

// The profile's options and UI_Options.fng's rows -- TASKS.md #259 item 8.
// (main.c is the layout half, exactly as for modeopts.h: this file says which
// rows a page has, what they read and write and what they show.)
//
// Where the settings live
// -----------------------
// PC keeps them in the UserProfile at 0x83A9E0, as OptionsSettings at +0x24
// (0x83AA04). That object's first word is the options page index the
// OptionsMain items write, and the four blocks after it are the ones the save
// stores whole (UserProfile::SaveToBuffer, docs/formats/savefile.md):
//
//   UP+0x28  VideoSettings     0x10   payload 0xC597
//   UP+0x38  GameplaySettings  0x10   payload 0xC537
//   UP+0x48  AudioSettings     0x3C   payload 0xC4FB
//   UP+0x84  PlayerSettings    2 x 0x28 (one per local player)  payload 0xC547
//
// The career difficulty is the CareerSettings word (UP+0x250, payload 0xC723)
// and the Save row is the two autosave bytes (UP+0xA0F8 / +0xA0F9); those two
// already have Profile fields of their own, so the rows read them there.
//
// Every field below is named after the row whose handler writes it; the ones
// no ported row touches keep the shipped default and round-trip untouched.
typedef struct {
	float f00, f04, f08; // the Display page's PC calibration values, 0.5/0/0
	u8 b0C;
	u8 pad[3];
} OptVideo;

typedef struct {
	u8 rearview;      // +0x0  Rearview Mirror, non-zero = On
	u8 b01;
	u8 speedoUnits;   // +0x2  0 = MPH, 1 = KM/H
	u8 raceMap;       // +0x3  Race Map Mode, 0 Off / 1 Static / 2 Rotate
	u8 exploreMap;    // +0x4  Explore Map Mode, the same three
	// +0x5 PORT (#471, the user's own): the GPS colour -- the arrow over the
	// road and the minimap's GPS rings -- an index into
	// options_gps_colour_rgb()'s table, 0 retail's blue. It sits in the
	// block's padding, which retail leaves 0 and the save copies whole, so a
	// save from before keeps the blue and the choice is per profile.
	u8 gpsColour;
	u8 pad[2];
	float jumpCam;    // +0x8  Jump Camera, 0 = Off
	float f0C;
} OptGameplay;

typedef struct {
	float master;     // +0x00 Master Volume
	float speech;     // +0x04 Speech & Movie Volume
	float feMusic;    // +0x08 Menu Music Volume
	float igMusic;    // +0x0C Game Music Volume
	float sfx;        // +0x10 Sound Effects Volume
	float engine;     // +0x14 Engine Volume
	float tire;       // +0x18 Tire Volume
	float f1C, f20, f24;
	u32 audioMode;    // +0x28 Audio Mode, 0 = Mono, 1 = Stereo
	u32 u2C;
	u32 playOrder;    // +0x30 EA Trax play order, 0 Sequential / 1 Random
	u32 u34, u38;
} OptAudio;

typedef struct {
	u8 gauges;        // +0x00
	u8 racePos;       // +0x01
	u8 lapInfo;       // +0x02
	u8 score;         // +0x03
	u8 feedback;      // +0x04 force feedback
	u8 leaderboard;   // +0x05
	u8 askTrans;      // +0x06 Ask for Transmission
	u8 b07;
	u32 u08;
	u8 b0C;
	u8 pad0D[3];
	u32 camera;       // +0x10 0 Bumper / 1 Hood / 2 Close / 3 Far
	u8 splitTime;     // +0x14 0 Leader / 1 Lap Record / 2 Best Lap / 3 Last Lap / 4 Off
	u8 transmission;  // +0x15 0 = Auto, 1 = Manual
	u8 stability;     // +0x16 0 = On, 1 = Off
	// +0x17 DragSteering (GC PlayerSettings +0x2B, 0x803BC2DB for player 0):
	// RaceStarter::SetupDragRace (0x8011B8F0) copies player 0's into the
	// global DragSteering, and Player::UpdateDragPilot (0x80217E44) puts the
	// drag pilot on only while it is 0 -- 3 there means each player's own.
	// The defaults leave it 0 (main.c's drag pilot).
	u8 dragSteering;
	u8 b18, b19;
	u8 pad1A[2];
	float f1C, f20, f24;
} OptPlayer;

#define OPTIONS_PLAYERS 2

// The EA Trax song table, UserProfile +0x2D8 (PC 0x83ACB8; GC the profile at
// 0x803BC210 +0x3DC), saved whole at payload 0xC423 just before
// AudioSettings. It is not part of OptionsSettings, but it is what the
// options' EA Trax page edits, so it travels with the four blocks here. The
// table's order is the playlist order the jukebox lists and the sequential
// pick walks; `song` is the index into the exe's track table (VA 0x7ECD80) and
// `play` the song's playability:
#define OPTIONS_TRAX_SONGS 27
#define OPTIONS_TRAX_OFF    0 // Off,    0x9CCE9F86
#define OPTIONS_TRAX_MENU   1 // Menu,   0x5278C50B -- the frontend rotation
#define OPTIONS_TRAX_RACING 2 // Racing, 0x5C1B351C -- the in-game rotation
#define OPTIONS_TRAX_ALL    3 // All,    0x9CCE64C4 -- both

typedef struct {
	u32 song;
	u8 play;
	u8 pad[3];
} OptTraxSong;

typedef struct {
	OptVideo video;
	OptGameplay gameplay;
	OptAudio audio;
	OptPlayer player[OPTIONS_PLAYERS];
	OptTraxSong trax[OPTIONS_TRAX_SONGS];
} ProfileOptions;

_Static_assert(sizeof(OptVideo) == 0x10, "VideoSettings is 0x10 bytes");
_Static_assert(sizeof(OptGameplay) == 0x10, "GameplaySettings is 0x10 bytes");
_Static_assert(sizeof(OptAudio) == 0x3C, "AudioSettings is 0x3C bytes");
_Static_assert(sizeof(OptPlayer) == 0x28, "PlayerSettings is 0x28 bytes");
_Static_assert(sizeof(OptTraxSong) * OPTIONS_TRAX_SONGS == 0xD8, "the EA Trax table is 0xD8 bytes");

// PORT (#471): the GPS colours the Gameplay page's GPS Color row steps
// through. 0 is retail's (the minimap rings' 0, 128, 255 and the arrow as
// baked); an index out of range reads as 0.
#define OPTIONS_GPS_COLOURS 10
u32 options_gps_colour_rgb(int index);        // 0xRRGGBB
const char* options_gps_colour_name(int index);

// SPEED2.EXE:0x5025B0, OptionsSettings' Default, all four blocks, plus the
// EA Trax table (options_default_trax). `language` is the only input: the
// speedometer starts on KM/H for every language but English (0).
void options_default(ProfileOptions* o, int language);

// Default__11UserProfile's EA Trax loop (0x515AA0, at 0x515B46): song i at
// position i, Menu for an FE song, Racing for IG, All for AL. The banks are
// the exe's own (the table's +0xC strings): songs 0-5 are FE, 6-26 IG.
void options_default_trax(OptTraxSong trax[OPTIONS_TRAX_SONGS]);

// PORT: a table that is not a permutation of 0..26 with playabilities 0..3.
// Retail never writes one; this port's saves from before the table was
// decoded carry zeros there.
bool options_trax_valid(const OptTraxSong trax[OPTIONS_TRAX_SONGS]);

// The pages the OptionsMain items write into OptionsSettings +0 (handlers
// 0x49A620..0x49A700), which UI_Options' ctor 0x4CF750 and Setup 0x4B8470
// switch on.
typedef enum {
	OPTPAGE_AUDIO = 0,
	OPTPAGE_DISPLAY = 1,
	OPTPAGE_GAMEPLAY = 2,
	OPTPAGE_PLAYER = 3,
	OPTPAGE_CONTROLS = 4,
	OPTPAGE_EATRAX = 5,
	OPTPAGE_TRAILERS = 6,
	OPTPAGE_NETWORK = 7,
	OPTPAGE_CREDITS = 8,
	// PORT: UI_PC_Customize_Options.fng, which the Display page's Customize
	// button reaches. Retail's screen is a class of its own (ctor 0x4CF660,
	// Setup 0x4B6510) rather than a page of UI_Options; the index is how
	// this port's row machinery, which keys on the page, gets its rows.
	OPTPAGE_CUSTOMIZE_DISPLAY = 9,
	// PORT (#448, the user's own): the hidden Debug Menu, a page of
	// UI_Options whose rows main.c supplies (options_set_debug_rows).
	OPTPAGE_DEBUG = 10,
} OptPage;

// One row class each, in builder order within a page.
typedef enum {
	// Audio, 0x4B6E60
	OPT_VOL_MASTER = 0,
	OPT_VOL_FE_MUSIC,
	OPT_VOL_IG_MUSIC,
	OPT_VOL_SPEECH,
	OPT_VOL_SFX,
	OPT_VOL_TIRE,
	OPT_VOL_ENGINE,
	OPT_AUDIO_MODE,
	// Gameplay, 0x4B7310
	OPT_JUMP_CAMERA,
	OPT_REARVIEW,
	OPT_SPEEDO_UNITS,
	OPT_CAREER_DIFFICULTY,
	OPT_EXPLORE_MAP,
	OPT_RACE_MAP,
	OPT_SAVE_MODE,
	// Player, 0x4B7CD0
	OPT_TRANSMISSION,
	OPT_STABILITY,
	OPT_CAMERA,
	OPT_GAUGES,
	OPT_RACE_POSITION,
	OPT_LAP_INFO,
	OPT_SPLIT_TIME,
	OPT_SCORE,
	OPT_LEADERBOARD,
	OPT_WHEEL_OPTIONS,
	OPT_FEEDBACK,
	OPT_ASK_TRANSMISSION,
	// Controls, Speed.elf's Setup__19UIOptionsController 0x800EF2F0. Its two
	// FEToggleWidgets are COConfig and COVibration (vtables 0x8038C860 and
	// 0x8038D570); each one's Draw writes the row's own label and value.
	OPT_CONTROL_TYPE,
	OPT_VIBRATION,
	// Display, 0x4B7180
	OPT_RESOLUTION,
	OPT_LEVEL_OF_DETAIL,
	OPT_CUSTOMIZE_DISPLAY,
	OPT_DEFAULT_DISPLAY,
	// PORT: stereoscopic 3D, on the Display page after the PC's four rows
	OPT_STEREO,
	OPT_STEREO_STYLE,  // #365: Into Screen / Pop Out
	OPT_STEREO_DEPTH,
	OPT_STEREO_FOCUS,
	OPT_STEREO_HUD,
	// Customize Display Settings, 0x4B6510. Each row reads and writes one
	// value of the display block (display.h), and is built only when that
	// value's cap is non-zero -- all but VSync, which has no cap.
	OPT_CD_ENVMAP,
	OPT_CD_ENV_UPDATE,
	OPT_CD_SHADOW,
	OPT_CD_HEADLIGHT,
	OPT_CD_CAR_LOD,
	OPT_CD_CROWD,
	OPT_CD_WORLD_LOD,
	OPT_CD_ROAD_REFL,
	OPT_CD_LIGHT_STREAKS,
	OPT_CD_LIGHT_GLOW,
	OPT_CD_PARTICLES,
	OPT_CD_MOTION_BLUR,
	OPT_CD_FOG,
	OPT_CD_DOF,
	OPT_CD_TINTING,
	OPT_CD_HORIZON_FOG,
	OPT_CD_OVER_BRIGHT,
	OPT_CD_BLEACH,
	OPT_CD_RAIN,
	OPT_CD_TEX_FILTER,
	OPT_CD_VSYNC,
	// PORT (#324, the user's setting): Car Damage on the Gameplay page, after
	// the PC's seven. English.bin still ships its label, OPT_GAMEPLAY_DAMAGE
	// 0x1582ADFF "Car Damage", which neither SPEED2.EXE nor Speed.elf uses --
	// a row cut from retail. Its value is the display block's own
	// g_CarDamageEnable (display.h), which the PC reads nowhere but its clamp
	// and its registry load and save (0x5BE690, 0x5BEA20, 0x5BEEA0).
	OPT_CAR_DAMAGE,
	// PORT (#471, the user's own): GPS Color, after Car Damage.
	OPT_GPS_COLOUR,
	// PORT (#447): the Controls page's rows, one per rebindable action in
	// controls.h's order (controls_action_hash), then Default Settings, then
	// (#527, the user's) Delete All Controls.
	OPT_BIND_FIRST,
	OPT_BIND_LAST = OPT_BIND_FIRST + 12,
	OPT_BIND_DEFAULT,
	OPT_BIND_CLEAR,
	// PORT (#448): the Debug Menu's rows, main.c's in its order.
	OPT_DBG_FIRST,
	OPT_DBG_LAST = OPT_DBG_FIRST + 15,
} OptKind;

#define OPT_IS_BIND(k) ((k) >= OPT_BIND_FIRST && (k) <= OPT_BIND_LAST)
#define OPT_IS_DBG(k) ((k) >= OPT_DBG_FIRST && (k) <= OPT_DBG_LAST)

// Customize Display Settings' 22 rows, when every cap is set.
#define OPTIONS_MAX_ROWS 22
_Static_assert(OPT_BIND_CLEAR - OPT_BIND_FIRST + 1 <= OPTIONS_MAX_ROWS,
	"the Controls page's rows fit");

typedef struct {
	OptKind kind;
	u32 labelHash; // English.bin key the row's refresh puts in OPTION_NAME_n
	bool slider;   // a slider row (AddRow 0x54F410) rather than a text row
	bool button;   // a select-only row with no value (the Wheel Options button)
	// PORT: the label of a row the PC does not have, whose name English.bin
	// has no key for (labelHash 0).
	const char* text;
} OptRow;

// What the rows read and write besides ProfileOptions. The difficulty word and
// the autosave pair are Profile fields; `named` is UserProfile +0x20, which
// the Save row's handler consults; `ctx` is the frontend context [0x850078].
// `display` is the display settings the Display pages edit (g_display).
typedef struct {
	ProfileOptions* opt;
	unsigned int* careerSettings;
	bool* autosave;
	bool* autosaveNoPrompt;
	bool named;
	int ctx;
	int player; // [0x836364], the player the Player page edits
	DisplaySettings* display;
} OptTarget;

// PORT (#448): the Debug Menu's rows. main.c owns the values (its Config's
// Dbg* fields and the globals they feed); options.c only lays them out as a
// page. Each row is a text row: LEFT / RIGHT step it, A steps it as RIGHT.
typedef struct {
	int (*count)(void);                 // rows, at most OPT_DBG_LAST - OPT_DBG_FIRST + 1
	const char* (*name)(int row);       // the row's label
	const char* (*value)(int row);      // the row's value, printed as is
	void (*adjust)(int row, bool right);
} OptDebugRows;
void options_set_debug_rows(const OptDebugRows* rows);

// The page's builder (Setup's table 0x4B85FC). Returns the row count; pages
// without a ported builder return 0.
int options_build_rows(OptPage page, const OptTarget* t, OptRow* rows, int maxRows);

// The row's refresh (vtable slot 3): a slider's position, 0..1, or the
// English.bin key of a text row's value (0 for a button). `number` is the one
// row whose value is printed rather than looked up -- COConfig::Draw
// (0x80308CBC) runs the scheme index through FEPrintf "%d" -- and is -1 on
// every other row. `text` is a value printed as a literal, as the Resolution
// row's refresh (0x49B690) prints "640x480" and the rest; NULL elsewhere.
typedef struct {
	float fraction;
	u32 valueHash;
	int number;
	const char* text;
} OptValue;
void options_row_value(const OptTarget* t, OptKind kind, OptValue* out);

// The row's LEFT/RIGHT handler (vtable slot 1). Returns true when what the row
// changed is a volume, i.e. when retail's handler goes on to UpdateVolumes
// (0x469050).
bool options_adjust(const OptTarget* t, OptKind kind, bool right);

// The row's accept (0x0C407210) through the same handler. Most rows ignore
// it; the Customize page's toggles flip on it as on LEFT and RIGHT, and the
// Display page's two buttons answer with what the screen does next.
typedef enum {
	OPTACCEPT_NONE,      // the row takes no accept
	OPTACCEPT_CHANGED,   // the row's value changed
	OPTACCEPT_CUSTOMIZE, // 0x49B930: FORWARD to UI_PC_Customize_Options.fng
	OPTACCEPT_DEFAULT,   // 0x49B980: the "Reset to default settings?" dialog
} OptAccept;
OptAccept options_accept(const OptTarget* t, OptKind kind);

// A row the builder created disabled: Feedback, whose enable is the
// force-feedback device byte [0x87078C] (0x5B8240) -- there is none on a 3DS.
bool options_row_disabled(OptKind kind);

// UI_Options' title per page (the ctor's table 0x4CFA98, widget 0x42ADB44C),
// 0 where the ctor writes none.
u32 options_page_title(OptPage page, int ctx);

// Every English.bin key a row value can resolve to; see modeopts_value_hashes.
#define OPTIONS_VALUE_HASH_COUNT 27
const u32* options_value_hashes(void);
int options_value_index(u32 hash);

// ---------------------------------------------------------------------------
// The Controls screen's button list
//
// UI_OptionsController.fng draws fourteen labels around a controller picture,
// CButton_1..CButton_14. Each label names a control on that picture, and the
// control scheme says which action that control performs:
//
//   SetupControllerButtons         0x800EF44C  walks CButton_1..14
//   FindButtonNameHashForFEString  0x80152C8C  the action for one label
//   PreFindButtonNameHashForFEString 0x80152BF8  the table lookup
//   DetectControllers              0x800EF560  which schemes the hardware has
//   COConfig::Act / ::Draw         0x80308B3C / 0x80308CBC  the Control Type row
//   PrepToShowControllerConfig     0x800EF748  the picture behind the labels
//
// The lookup is one load: 0x8035E2C8[slot * 0x24 + scheme * 4], fourteen slots
// by nine schemes. A wheel reads one of two tables of its own instead
// (0x8035E4C0 without pedals, 0x8035E5D8 with, five schemes of 0x14 and only
// two filled); this console has no wheel, so neither is carried here.
//
// The nine schemes are two sets. Schemes 0-4 steer with the analogue stick;
// 5-8 steer with the D-pad, which is why Steer moves from slot 3 to slot 7
// across them and carries a second English.bin key there. DetectControllers
// picks the set from the pad: a pad with a stick stays in 0-4 (and COConfig's
// Act then steps 0..3, Draw printing scheme + 1), a digital one is pushed into
// 5-8 (Act steps 5..8, Draw printing scheme - 4). A 3DS always has a Circle
// Pad, so this port is always the first case: schemes 0-3, Control Type 1-4.
//
// PORT: the picture is a PS2 DualShock, the wrong hardware for this console,
// so main.c hides it and writes its own 3DS control into each label instead --
// see kControlSlot there for which control sits at which slot and why. The
// control belongs to the slot, as retail's picture does; the scheme only moves
// actions between slots, which is exactly what makes Control Type work here.
#define OPTIONS_CONTROL_BUTTONS 14
#define OPTIONS_CONTROL_SCHEMES 9

// The actions the table names, by their English.bin key. ACT_STEER_DIGITAL is
// the second "Steer" entry, the one schemes 5-8 use; ACT_PAUSE appears only in
// the wheel tables.
#define OPT_ACT_CHANGE_CAMERA 0xD38DF194u // "Change Camera"
#define OPT_ACT_ACCELERATE    0x619B23B0u // "Accelerate"
#define OPT_ACT_BRAKE         0x6AB80AFAu // "Brake / Reverse"
#define OPT_ACT_STEER         0xECD20F7Fu // "Steer"
#define OPT_ACT_STEER_DIGITAL 0x63AA639Du // "Steer", schemes 5-8
#define OPT_ACT_ENTER_SHOP    0xAB1032FAu // "Enter Shop /^Challenge Opponent"
#define OPT_ACT_WORLD_MAP     0x6026C91Fu // "World Map"
#define OPT_ACT_CAREER_STATS  0x70A30056u // "Career Statistics"
#define OPT_ACT_DISPLAY_SMS   0x6EF09D0Du // "Display SMS"
#define OPT_ACT_SHIFT_UP      0xDC1CD97Du // "Shift Up"
#define OPT_ACT_SHIFT_DOWN    0x56AFE190u // "Shift Down"
#define OPT_ACT_HANDBRAKE     0xB70FF4A4u // "Handbrake"
#define OPT_ACT_NITROUS       0x5E5C5F2Eu // "Nitrous"
#define OPT_ACT_RESET_CAR     0x1CEDC132u // "Reset Car"
#define OPT_ACT_LOOK_BACK     0x13121940u // "Look Back"
#define OPT_ACT_PAUSE         0x6369F4B8u // "Pause", the wheel tables only

// Retail's own word for a slot with no action behind it, which 0x80152CF8
// substitutes for the whole label. This port uses it for the second line of an
// action it does not act on -- the same statement about the same screen, made
// where the port's binding would otherwise go.
#define OPTIONS_NOT_USED_HASH 0xF8B4303Bu // "Not Used"

// CButton_N's own name hash, FEHashUpper("CButton_1") and so on, for slot 0..13.
u32 options_control_object(int slot);

// 0x8035E2C8[slot][scheme], the action that slot performs. `freeRoam` is
// SetupControllerButtons' fourth argument inverted for readability: retail
// clears it when the game mode (FEDatabase+0x125F0) is online (8) or LAN
// (0x200), and FindButtonNameHashForFEString then answers "Not Used" for the
// four actions free roam owns -- Display SMS, World Map, Career Statistics and
// Enter Shop. This port has neither mode, so main.c always passes true.
u32 options_control_action(int slot, int scheme, bool freeRoam);

// The scheme the Controls screen edits. Retail keeps it in FEDatabase
// (+0xBC, one word per player at a 0x40 stride) and not in any of the settings
// blocks the save stores, so it lasts the session and starts at 0 -- which is
// what this port does too.
int options_control_scheme(int player);
void options_control_set_scheme(int player, int scheme);

// COConfig::Act's range for a pad with an analogue stick, which is the only
// kind this console has: 0 and 3. Draw prints scheme + 1.
#define OPTIONS_CONTROL_SCHEME_LO 0
#define OPTIONS_CONTROL_SCHEME_HI 3
