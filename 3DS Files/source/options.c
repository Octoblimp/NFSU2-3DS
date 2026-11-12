#include "options.h"
#include <string.h>

#include "controls.h"

// ---------------------------------------------------------------------------
// English.bin keys
//
// As in modeopts.c: every hash was read out of the row class it belongs to and
// looked up in the shipped LANGUAGES/English.bin; the comment is that lookup.
// ---------------------------------------------------------------------------

// Audio, builder 0x4B6E60 (sliders, AddRow 0x54F410)
#define LBL_MASTER_VOL 0x26782C3Eu   // "Master Volume"
#define LBL_FE_MUSIC_VOL 0x418E681Du // "Menu Music Volume"
#define LBL_IG_MUSIC_VOL 0xDF21EAC2u // "Game Music Volume"
#define LBL_SPEECH_VOL 0x9E5FB82Au   // "Speech & Movie Volume"
#define LBL_SFX_VOL 0xFD487543u      // "Sound Effects Volume"
#define LBL_TIRE_VOL 0x69B745C6u     // "Tire Volume"
#define LBL_ENGINE_VOL 0xA2B1F888u   // "Engine Volume"
#define LBL_AUDIO_MODE 0x2881AB87u   // "Audio Mode"        refresh 0x49ABD0

// Gameplay, builder 0x4B7310
#define LBL_JUMP_CAMERA 0xF26A5CBFu  // "Jump Camera"       refresh 0x49ADC0
#define LBL_REARVIEW 0x85A6CE05u     // "Rearview Mirror"   refresh 0x49AE40
#define LBL_SPEEDO 0xF86A6272u       // "Speedometer Units" refresh 0x49AEE0
#define LBL_CAREER_DIFF 0xBC0E6C55u  // "Career Difficulty" refresh 0x49AF70
#define LBL_EXPLORE_MAP 0xC6269082u  // "Explore Map Mode"  refresh 0x49B100
#define LBL_RACE_MAP 0x9FA5EC9Eu     // "Race Map Mode"     refresh 0x49B040
#define LBL_SAVE 0x996463E6u         // "Save"              refresh 0x49B210
#define LBL_CAR_DAMAGE 0x1582ADFFu   // "Car Damage", OPT_GAMEPLAY_DAMAGE (PORT, #324)

// Player, builder 0x4B7CD0
#define LBL_TRANSMISSION 0xD31407E7u // "Transmission"      refresh 0x4B7630
#define LBL_STABILITY 0xDF06ED22u    // "Stability Control" refresh 0x4B76F0
#define LBL_CAMERA 0xF6CCDC5Fu       // "Camera"            refresh 0x4B77B0
#define LBL_GAUGES 0xAC148579u       // "Gauges"
#define LBL_RACE_POSITION 0x82CD8F92u // "Race Position"
#define LBL_LAP_INFO 0x969F96A5u     // "Lap Info"
#define LBL_SPLIT_TIME 0x084BC378u   // "Split Time"
#define LBL_SCORE 0xC03F9F19u        // "Score"
#define LBL_LEADERBOARD 0xC93FBFB5u  // "Leaderboard"
#define LBL_WHEEL_OPTIONS 0x4DEAB931u // "Wheel Options"
#define LBL_FEEDBACK 0x358CDCFDu     // "Feedback:"
#define LBL_ASK_TRANS 0xBFB34120u    // "Ask for Transmission"

// Controls, Setup__19UIOptionsController 0x800EF2F0. Both labels come out of
// the row class's own Draw rather than a builder table: COConfig::Draw
// (0x80308CBC) and COVibration::Draw (0x800EC51C) each set their name string
// with FEngSetLanguageHash before writing the value.
#define LBL_CONTROL_TYPE 0xBA7306AAu // "Control Type"      Draw 0x80308CBC
#define LBL_VIBRATION 0x358CDBD4u    // "Rumble Feature:"   Draw 0x800EC51C

// Display, builder 0x4B7180
#define LBL_RESOLUTION 0xA9A76BBAu   // "Resolution"        refresh 0x49B690
#define LBL_LOD 0xB55E7665u          // "Level Of Detail"   refresh 0x49B7A0
#define LBL_CUSTOMIZE 0x381FE1E9u    // "Customize Display Settings" 0x49B960
#define LBL_DEFAULT 0x13F0EDEBu      // "Default Settings"  refresh 0x49B9E0
// PORT: the stereo rows. "3-D" is the shipped string; English.bin has no
// key for the other three, so they carry their own labels.
#define LBL_STEREO 0x496817D5u       // "3-D"

// Customize Display Settings, builder 0x4B6510 (each row's refresh)
#define LBL_CD_ENVMAP 0x7D3930E7u    // "Car Reflection Update Rate" 0x49BA50
#define LBL_CD_ENV_UPDATE 0x8FE9288Eu // "Car Reflection Detail"     0x49BAF0
#define LBL_CD_SHADOW 0xA7A683ACu    // "Car Shadow / Neon"          0x49BB70
#define LBL_CD_HEADLIGHT 0xCFBEDB50u // "Car Headlight"              0x49BC10
#define LBL_CD_CAR_LOD 0x4AC50BCFu   // "Car Geometry Detail"        0x49BC90
#define LBL_CD_CROWD 0x06CC3750u     // "Crowds"                     0x49BD10
#define LBL_CD_WORLD_LOD 0xBDE2E94Bu // "World Detail"               0x49BD90
#define LBL_CD_ROAD_REFL 0x2A2A4AB3u // "Road Reflection Detail"     0x49BE40
#define LBL_CD_STREAKS 0x6F2C7417u   // "Light Trails"               0x49BEF0
#define LBL_CD_GLOW 0x62925361u      // "Light Glow"                 0x49BF80
#define LBL_CD_PARTICLES 0x2522A4E9u // "Particle System"            0x49C000
#define LBL_CD_MOTION_BLUR 0x822E4E9Bu // "Motion Blur"              0x49C090
#define LBL_CD_FOG 0x95B8F38Du       // "Fog"                        0x49C110
#define LBL_CD_DOF 0xA9C27DFDu       // "Depth Of Field"             0x49C260
#define LBL_CD_TINTING 0x11504D0Eu   // "Tinting"                    0x49C2E0
#define LBL_CD_HORIZON_FOG 0x4E57D355u // "Horizon Fog"              0x49C360
#define LBL_CD_OVER_BRIGHT 0xD6F5F38Cu // "Over Bright"              0x49C3F0
#define LBL_CD_BLEACH 0x571F6C00u    // "Enhanced Contrast"          0x49C480
#define LBL_CD_RAIN 0x4CDDBEBBu      // "Rain Splatter"              0x49C500
#define LBL_CD_TEX_FILTER 0x61A50B8Fu // "Texture Filtering"         0x49B840
#define LBL_CD_VSYNC 0x5B4AA6E4u     // "Vsync"                      0x49B8E0

#define VAL_ON 0x0000063Cu           // "On"
#define VAL_OFF 0x0000CCFAu          // "Off"
#define VAL_MPH 0x8569AB44u          // "MPH"
#define VAL_KMH 0x8569A25Fu          // "KM/H"
#define VAL_EASY 0xDBBB4E8Fu         // "Easy"
#define VAL_MEDIUM 0xCAB262DEu       // "Medium"
#define VAL_HARD 0xDBBCF37Cu         // "Hard"
#define VAL_STATIC 0xF75595F2u       // "Static"
#define VAL_ROTATE 0xF4B00E99u       // "Rotate"
#define VAL_SAVE_AUTO 0x1B7ABF8Eu    // "Autosave"
#define VAL_SAVE_PROMPT 0x1B7ABF8Fu  // "Prompt"
#define VAL_SAVE_OFF 0x1B7ABF90u     // "Off"
#define VAL_MONO 0xC50FA35Fu         // "Mono"
#define VAL_STEREO 0x55DA8BF8u       // "Stereo"
#define VAL_AUTO 0x61CB0720u         // "Auto"
#define VAL_MANUAL 0x1B3A2485u       // "Manual"
#define VAL_BUMPER 0xC3E9AE58u       // "Bumper"
#define VAL_HOOD 0x414F19D7u         // "Hood"
#define VAL_CLOSE 0x5AE3441Fu        // "Close"
#define VAL_FAR 0x1EA4CEC2u          // "Far"
#define VAL_SUPER_FAR 0x916039B4u    // "Super Far"
#define VAL_LEADER 0xD93F8F55u       // "Leader"
#define VAL_LAP_RECORD 0xC44D3943u   // "Lap Record"
#define VAL_BEST_LAP 0x17FAFC32u     // "Best Lap"
#define VAL_LAST_LAP 0x1EA459F8u     // "Last Lap"
#define VAL_HIGH 0x612AF8A6u         // "High"
#define VAL_LOW 0xB55E7678u          // "Low"

// Page titles, the ctor's table 0x4CFA98 (widget 0x42ADB44C)
#define TTL_AUDIO 0xE76CD783u        // "Audio"
#define TTL_DISPLAY 0x4204D527u      // "Display"
#define TTL_GAMEPLAY 0xD0CF6EE1u     // "Gameplay"
#define TTL_PLAYER 0xF760EABEu       // "Player"
#define TTL_EATRAX 0xDCFB6B36u       // "EA GAMES TRAX"
#define TTL_TRAILERS 0xD10D7477u     // "Trailers"
#define TTL_LAN 0x19889765u          // "LAN Options"
#define TTL_ONLINE 0x22B1C2E6u       // "Online Options"
#define TTL_CREDITS 0x0905101Fu      // "Credits"
// The Controls screen titles itself in its own ctor (0x800EEE28) rather than
// from UI_Options' table: FEngFindString(0x42ADB44C) -> 0x04DC6DB5.
#define TTL_CONTROLS 0x04DC6DB5u     // "Controls"
// Customize Display Settings' ctor (0x4CF660) writes its title into the same
// slot: the Display page's button label.
#define TTL_CUSTOMIZE LBL_CUSTOMIZE

// The frontend context the Race Map row and the Audio builder test.
#define CTX_SPLITSCREEN 4
#define CTX_ONLINE 8

// ---------------------------------------------------------------------------
// Defaults -- SPEED2.EXE:0x5025B0
// ---------------------------------------------------------------------------

// Called on OptionsSettings (UP+0x24), so every offset in the comments below is
// relative to that and the blocks start at +0x04/+0x14/+0x24/+0x60/+0x88. The
// function writes every field this struct models except Player +0x07 and the
// pads; those are BSS zero, which the memset stands for.
static void defaultPlayer(OptPlayer* p) {
	p->gauges = 1;       // +0x60
	p->racePos = 1;      // +0x61
	p->lapInfo = 1;      // +0x62
	p->score = 1;        // +0x63
	p->feedback = 1;     // +0x64
	p->leaderboard = 1;  // +0x65
	p->askTrans = 1;     // +0x66
	p->u08 = 0;          // +0x68
	p->b0C = 0;          // +0x6C
	p->camera = 3;       // +0x70  Far
	p->splitTime = 0;    // +0x74  Leader
	p->transmission = 0; // +0x75  Auto
	p->stability = 1;    // +0x76
	p->dragSteering = 0; // +0x77
	p->b18 = 1;          // +0x78
	p->b19 = 0;          // +0x79
	p->f1C = 1.0f;       // +0x7C
	p->f20 = 12.4f;      // +0x80  0x41466666
	p->f24 = 1.0f;       // +0x84
}

// PORT (#471). Blue first: retail's.
static const struct {
	const char* name;
	u32 rgb;
} kGpsColours[OPTIONS_GPS_COLOURS] = {
	{ "Blue",   0x0080FFu },
	{ "Cyan",   0x00E0FFu },
	{ "Green",  0x00C040u },
	{ "Lime",   0x9CFF00u },
	{ "Yellow", 0xFFE000u },
	{ "Orange", 0xFF8000u },
	{ "Red",    0xFF2020u },
	{ "Pink",   0xFF40C0u },
	{ "Purple", 0x9040FFu },
	{ "White",  0xFFFFFFu },
};

u32 options_gps_colour_rgb(int index) {
	return kGpsColours[(index >= 0 && index < OPTIONS_GPS_COLOURS) ? index : 0].rgb;
}

const char* options_gps_colour_name(int index) {
	return kGpsColours[(index >= 0 && index < OPTIONS_GPS_COLOURS) ? index : 0].name;
}

void options_default(ProfileOptions* o, int language) {
	memset(o, 0, sizeof(*o));

	o->video.f00 = 0.5f; // +0x04
	o->video.f04 = 0.0f; // +0x08
	o->video.f08 = 0.0f; // +0x0C
	o->video.b0C = 0;    // +0x10

	o->gameplay.rearview = 1;                        // +0x14
	o->gameplay.b01 = 0;                             // +0x15
	o->gameplay.speedoUnits = (language != 0) ? 1 : 0; // +0x16, [0x7F70D0] != 0
	o->gameplay.raceMap = 2;                         // +0x17  Rotate
	o->gameplay.exploreMap = 2;                      // +0x18  Rotate
	o->gameplay.jumpCam = 127.5f;                    // +0x1C  0x42FF0000
	o->gameplay.f0C = 127.5f;                        // +0x20

	o->audio.master = 1.0f;  // +0x24
	o->audio.speech = 1.0f;  // +0x28
	o->audio.feMusic = 0.7f; // +0x2C  0x3F333333
	o->audio.igMusic = 1.0f; // +0x30
	o->audio.sfx = 1.0f;     // +0x34
	o->audio.engine = 1.0f;  // +0x38
	o->audio.tire = 1.0f;    // +0x3C
	o->audio.f1C = 1.0f;     // +0x40
	o->audio.f20 = 1.0f;     // +0x44
	o->audio.f24 = 1.0f;     // +0x48
	o->audio.audioMode = 1;  // +0x4C  Stereo
	o->audio.u2C = 0;        // +0x50
	o->audio.playOrder = 1;  // +0x54  Random
	o->audio.u34 = 0;        // +0x58
	o->audio.u38 = 0;        // +0x5C

	for (int i = 0; i < OPTIONS_PLAYERS; i++)
		defaultPlayer(&o->player[i]);

	options_default_trax(o->trax);
}

// The banks of the exe's track table, record +0xC (VA 0x7ECD80).
#define TRAX_FE_SONGS 6

void options_default_trax(OptTraxSong trax[OPTIONS_TRAX_SONGS]) {
	memset(trax, 0, sizeof(OptTraxSong) * OPTIONS_TRAX_SONGS);
	for (int i = 0; i < OPTIONS_TRAX_SONGS; i++) {
		trax[i].song = (u32)i;
		trax[i].play = (i < TRAX_FE_SONGS) ? OPTIONS_TRAX_MENU : OPTIONS_TRAX_RACING;
	}
}

bool options_trax_valid(const OptTraxSong trax[OPTIONS_TRAX_SONGS]) {
	u32 seen = 0;
	for (int i = 0; i < OPTIONS_TRAX_SONGS; i++) {
		if (trax[i].song >= OPTIONS_TRAX_SONGS || trax[i].play > OPTIONS_TRAX_ALL)
			return false;
		if (seen & (1u << trax[i].song))
			return false;
		seen |= 1u << trax[i].song;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Row building -- Setup's table 0x4B85FC
// ---------------------------------------------------------------------------

typedef struct {
	OptRow* rows;
	int n;
	int max;
} RowSink;

static void addRow(RowSink* sink, OptKind kind, u32 labelHash, bool slider, bool button) {
	if (sink->n >= sink->max)
		return;
	sink->rows[sink->n].kind = kind;
	sink->rows[sink->n].labelHash = labelHash;
	sink->rows[sink->n].slider = slider;
	sink->rows[sink->n].button = button;
	sink->rows[sink->n].text = NULL;
	sink->n++;
}

// PORT: a row whose label is not in English.bin.
static void addTextRow(RowSink* sink, OptKind kind, const char* text, bool slider) {
	if (sink->n >= sink->max)
		return;
	addRow(sink, kind, 0, slider, false);
	sink->rows[sink->n - 1].text = text;
}

// SPEED2.EXE:0x4B6E60. Seven sliders in this order (the vtables 0x7918F8..
// 0x7919E8 bind each one to its AudioSettings field), then Audio Mode, which
// 0x4B7102 leaves out of an online session.
static void buildAudio(const OptTarget* t, RowSink* sink) {
	addRow(sink, OPT_VOL_MASTER, LBL_MASTER_VOL, true, false);
	addRow(sink, OPT_VOL_FE_MUSIC, LBL_FE_MUSIC_VOL, true, false);
	addRow(sink, OPT_VOL_IG_MUSIC, LBL_IG_MUSIC_VOL, true, false);
	addRow(sink, OPT_VOL_SPEECH, LBL_SPEECH_VOL, true, false);
	addRow(sink, OPT_VOL_SFX, LBL_SFX_VOL, true, false);
	addRow(sink, OPT_VOL_TIRE, LBL_TIRE_VOL, true, false);
	addRow(sink, OPT_VOL_ENGINE, LBL_ENGINE_VOL, true, false);
	if (t->ctx != CTX_ONLINE)
		addRow(sink, OPT_AUDIO_MODE, LBL_AUDIO_MODE, false, false);
}

// SPEED2.EXE:0x4B7310 -- seven rows, none of them gated.
static void buildGameplay(const OptTarget* t, RowSink* sink) {
	(void)t;
	addRow(sink, OPT_JUMP_CAMERA, LBL_JUMP_CAMERA, false, false);  // 0x791C00
	addRow(sink, OPT_REARVIEW, LBL_REARVIEW, false, false);        // 0x791C48
	addRow(sink, OPT_SPEEDO_UNITS, LBL_SPEEDO, false, false);      // 0x791C90
	addRow(sink, OPT_CAREER_DIFFICULTY, LBL_CAREER_DIFF, false, false); // 0x791D20
	addRow(sink, OPT_EXPLORE_MAP, LBL_EXPLORE_MAP, false, false);  // 0x791D68
	addRow(sink, OPT_RACE_MAP, LBL_RACE_MAP, false, false);        // 0x791CD8
	addRow(sink, OPT_SAVE_MODE, LBL_SAVE, false, false);           // 0x793D68
	// PORT (#324): retail's cut Car Damage row, the profile's .display file's
	// g_CarDamageEnable. Off is retail: the damage shows only when the crash
	// camera smashes the car or a drag totals it (main.c carDamageUpdate()).
	if (t->display)
		addRow(sink, OPT_CAR_DAMAGE, LBL_CAR_DAMAGE, false, false);
	// PORT (#471, the user's own): the GPS arrow's and the minimap rings'
	// colour, per profile. English.bin has no key for it.
	addTextRow(sink, OPT_GPS_COLOUR, "GPS Color", false);
}

// SPEED2.EXE:0x4B7CD0 -- twelve rows for player [0x836364], none of them gated
// (Feedback is built disabled, see options_row_disabled).
static void buildPlayer(const OptTarget* t, RowSink* sink) {
	(void)t;
	addRow(sink, OPT_TRANSMISSION, LBL_TRANSMISSION, false, false); // 0x7940F0
	addRow(sink, OPT_STABILITY, LBL_STABILITY, false, false);       // 0x794138
	addRow(sink, OPT_CAMERA, LBL_CAMERA, false, false);             // 0x794180
	addRow(sink, OPT_GAUGES, LBL_GAUGES, false, false);             // 0x7941C8
	addRow(sink, OPT_RACE_POSITION, LBL_RACE_POSITION, false, false); // 0x794210
	addRow(sink, OPT_LAP_INFO, LBL_LAP_INFO, false, false);         // 0x794258
	addRow(sink, OPT_SPLIT_TIME, LBL_SPLIT_TIME, false, false);     // 0x7942E8
	addRow(sink, OPT_SCORE, LBL_SCORE, false, false);               // 0x7942A0
	addRow(sink, OPT_LEADERBOARD, LBL_LEADERBOARD, false, false);   // 0x794330
	addRow(sink, OPT_WHEEL_OPTIONS, LBL_WHEEL_OPTIONS, false, true); // 0x791E00
	addRow(sink, OPT_FEEDBACK, LBL_FEEDBACK, false, false);         // 0x791DB0
	addRow(sink, OPT_ASK_TRANSMISSION, LBL_ASK_TRANS, false, false); // 0x794378
}

// Speed.elf:0x800EF2F0 builds two rows, the COConfig toggle (Control Type,
// four fixed schemes) and COVibration (Rumble Feature).
//
// PORT (#447, the user's ask): the remappable controls instead -- one row per
// action, named as retail's Controls screen names it, whose value is the
// button it is on (LEFT / RIGHT and A move it, swapping with the action on
// that button; controls.h), then Default Settings, which puts back retail's
// scheme 0. Rumble Feature goes: there is no motor in a 3DS to drive.
// Enter Shop's English.bin name breaks its line ("Enter Shop /^Challenge
// Opponent"), which a row label cannot, so that row says it on one line.
static void buildControls(const OptTarget* t, RowSink* sink) {
	(void)t;
	for (int i = 0; i < CONTROLS_ACTIONS; i++) {
		const u32 h = controls_action_hash(i);
		if (h == OPT_ACT_ENTER_SHOP)
			addTextRow(sink, (OptKind)(OPT_BIND_FIRST + i), "Enter Shop / Challenge", false);
		else
			addRow(sink, (OptKind)(OPT_BIND_FIRST + i), h, false, false);
	}
	addRow(sink, OPT_BIND_DEFAULT, LBL_DEFAULT, false, true);
	// #527 (the user: "add something at the bottom that says Delete All
	// Controls that just removes all controls for you"): a button, no question.
	addTextRow(sink, OPT_BIND_CLEAR, "Delete All Controls", false);
	sink->rows[sink->n - 1].button = true;   // a button, as Default Settings is
}

// SPEED2.EXE:0x4B7180 -- four rows, none of them gated: Resolution, the Level
// of Detail slider, then the Customize and Default buttons (AddButton
// 0x54EE40).
//
// PORT: Resolution is built disabled. Its LEFT/RIGHT (0x49B600) steps the
// card's mode list; the 3DS's top screen has the one mode, which is what the
// row prints. After the PC's four come the stereoscopic 3D rows, which the
// PC has no use for: 3-D on or off, then how deep the picture goes at full
// slider, how far ahead the screen's plane sits, and how far the HUD stands
// out (display.h).
static void buildDisplay(const OptTarget* t, RowSink* sink) {
	if (!t->display)
		return;
	addRow(sink, OPT_RESOLUTION, LBL_RESOLUTION, false, false);  // 0x791F30
	addRow(sink, OPT_LEVEL_OF_DETAIL, LBL_LOD, true, false);    // 0x791F78
	addRow(sink, OPT_CUSTOMIZE_DISPLAY, LBL_CUSTOMIZE, false, true); // 0x792060
	addRow(sink, OPT_DEFAULT_DISPLAY, LBL_DEFAULT, false, true); // 0x7920A0
	addRow(sink, OPT_STEREO, LBL_STEREO, false, false);
	addTextRow(sink, OPT_STEREO_STYLE, "3-D Style", false);
	addTextRow(sink, OPT_STEREO_DEPTH, "3-D Depth", true);
	addTextRow(sink, OPT_STEREO_FOCUS, "3-D Focus", true);
	addTextRow(sink, OPT_STEREO_HUD, "3-D HUD Depth", true);
}

// The Customize page's rows: the display-block value each one edits (an index
// in u32s, display.h), and whether it is a slider (AddRow 0x54F410, 0..cap)
// or a toggle that prints High/Low rather than On/Off.
typedef struct {
	OptKind kind;
	u32 label;
	int index;
	bool slider;
	bool highLow;
} CustomizeRow;

// In the builder's order, with each row's vtable.
static const CustomizeRow kCustomize[] = {
	{ OPT_CD_ENVMAP, LBL_CD_ENVMAP, 0x08 / 4, true, false },        // 0x7920E0
	{ OPT_CD_ENV_UPDATE, LBL_CD_ENV_UPDATE, 0x0C / 4, false, true }, // 0x792130
	{ OPT_CD_SHADOW, LBL_CD_SHADOW, 0x10 / 4, true, false },        // 0x792178
	{ OPT_CD_HEADLIGHT, LBL_CD_HEADLIGHT, 0x14 / 4, false, false }, // 0x7921C8
	{ OPT_CD_CAR_LOD, LBL_CD_CAR_LOD, 0x48 / 4, false, true },      // 0x792210
	{ OPT_CD_CROWD, LBL_CD_CROWD, 0x20 / 4, false, false },         // 0x792258
	{ OPT_CD_WORLD_LOD, LBL_CD_WORLD_LOD, 0x44 / 4, true, false },  // 0x7922A0
	{ OPT_CD_ROAD_REFL, LBL_CD_ROAD_REFL, 0x24 / 4, true, false },  // 0x7922F0
	{ OPT_CD_LIGHT_STREAKS, LBL_CD_STREAKS, 0x30 / 4, false, false }, // 0x792340
	{ OPT_CD_LIGHT_GLOW, LBL_CD_GLOW, 0x34 / 4, false, false },     // 0x792388
	{ OPT_CD_PARTICLES, LBL_CD_PARTICLES, 0x3C / 4, false, false }, // 0x7923D0
	{ OPT_CD_MOTION_BLUR, LBL_CD_MOTION_BLUR, 0x2C / 4, false, false }, // 0x792418
	{ OPT_CD_FOG, LBL_CD_FOG, 0x28 / 4, false, false },             // 0x792460
	{ OPT_CD_DOF, LBL_CD_DOF, 0x40 / 4, false, false },             // 0x7924F8
	// The PC's Full Screen Anti-Aliasing row (0x7924A8) is cut (#473, the
	// user 2026-11-09: "Cut FSAA, leave light trails.").
	{ OPT_CD_TINTING, LBL_CD_TINTING, 0x54 / 4, false, false },     // 0x792540
	{ OPT_CD_HORIZON_FOG, LBL_CD_HORIZON_FOG, 0x5C / 4, false, false }, // 0x792588
	{ OPT_CD_OVER_BRIGHT, LBL_CD_OVER_BRIGHT, 0x4C / 4, false, false }, // 0x7925D0
	{ OPT_CD_BLEACH, LBL_CD_BLEACH, 0x50 / 4, false, false },       // 0x792618
	{ OPT_CD_RAIN, LBL_CD_RAIN, 0x60 / 4, false, false },           // 0x792660
	{ OPT_CD_TEX_FILTER, LBL_CD_TEX_FILTER, 0x68 / 4, true, false }, // 0x791FC8
};
#define CUSTOMIZE_ROWS ((int)(sizeof(kCustomize) / sizeof(kCustomize[0])))

static const CustomizeRow* customizeRow(OptKind kind) {
	for (int i = 0; i < CUSTOMIZE_ROWS; i++)
		if (kCustomize[i].kind == kind)
			return &kCustomize[i];
	return NULL;
}

// SPEED2.EXE:0x4B6510 -- a row per value whose cap is non-zero (the caps
// table at 0x8764D0, which display_cap answers for this port), then Vsync,
// which no cap gates. The first row also adds a blank one under itself when
// its label has a '^' line break in it; the English one has none.
//
// PORT: Vsync is built disabled. Its LEFT/RIGHT (0x49B8A0) sets the Direct3D
// present interval; the 3DS swaps its framebuffers at vblank only, so the
// row prints On, as Rumble Feature prints Off for the motor a 3DS lacks.
static void buildCustomizeDisplay(const OptTarget* t, RowSink* sink) {
	if (!t->display)
		return;
	for (int i = 0; i < CUSTOMIZE_ROWS; i++)
		if (display_cap(kCustomize[i].index) != 0)
			addRow(sink, kCustomize[i].kind, kCustomize[i].label, kCustomize[i].slider, false);
	addRow(sink, OPT_CD_VSYNC, LBL_CD_VSYNC, false, false);         // 0x792018
}

// PORT (#448): the Debug Menu's rows, from main.c.
static const OptDebugRows* s_debugRows = NULL;

void options_set_debug_rows(const OptDebugRows* rows) {
	s_debugRows = rows;
}

static void buildDebug(RowSink* sink) {
	if (!s_debugRows || !s_debugRows->count)
		return;
	int n = s_debugRows->count();
	if (n > OPT_DBG_LAST - OPT_DBG_FIRST + 1)
		n = OPT_DBG_LAST - OPT_DBG_FIRST + 1;
	for (int i = 0; i < n; i++)
		addTextRow(sink, (OptKind)(OPT_DBG_FIRST + i), s_debugRows->name(i), false);
}

int options_build_rows(OptPage page, const OptTarget* t, OptRow* rows, int maxRows) {
	RowSink sink = { rows, 0, maxRows };
	if (!t || !t->opt || !rows || maxRows <= 0)
		return 0;
	switch (page) {
	case OPTPAGE_AUDIO: buildAudio(t, &sink); break;
	case OPTPAGE_DISPLAY: buildDisplay(t, &sink); break;
	case OPTPAGE_GAMEPLAY: buildGameplay(t, &sink); break;
	case OPTPAGE_PLAYER: buildPlayer(t, &sink); break;
	case OPTPAGE_CONTROLS: buildControls(t, &sink); break;
	case OPTPAGE_CUSTOMIZE_DISPLAY: buildCustomizeDisplay(t, &sink); break;
	case OPTPAGE_DEBUG: buildDebug(&sink); break;
	default: break; // Network: not ported (see TASKS.md #259)
	}
	return sink.n;
}

bool options_row_disabled(OptKind kind) {
	// Feedback's enable is the force-feedback device byte [0x87078C]
	// (0x5B8240); Rumble Feature, Resolution and Vsync are disabled for the
	// reasons on their builders. None has hardware behind it here.
	return kind == OPT_FEEDBACK || kind == OPT_VIBRATION || kind == OPT_RESOLUTION ||
		kind == OPT_CD_VSYNC;
}

// ---------------------------------------------------------------------------
// Row values -- the refresh of each row class
// ---------------------------------------------------------------------------

static OptPlayer* playerOf(const OptTarget* t) {
	int p = t->player;
	if (p < 0 || p >= OPTIONS_PLAYERS)
		p = 0;
	return &t->opt->player[p];
}

static float* volumeOf(const OptTarget* t, OptKind kind) {
	OptAudio* a = &t->opt->audio;
	switch (kind) {
	case OPT_VOL_MASTER: return &a->master;    // +0x00
	case OPT_VOL_FE_MUSIC: return &a->feMusic; // +0x08
	case OPT_VOL_IG_MUSIC: return &a->igMusic; // +0x0C
	case OPT_VOL_SPEECH: return &a->speech;    // +0x04
	case OPT_VOL_SFX: return &a->sfx;          // +0x10
	case OPT_VOL_TIRE: return &a->tire;        // +0x18
	case OPT_VOL_ENGINE: return &a->engine;    // +0x14
	default: return NULL;
	}
}

// The Player page's plain toggles all share one handler shape (test, sete) and
// one refresh (non-zero = On).
static u8* toggleOf(const OptTarget* t, OptKind kind) {
	OptPlayer* p = playerOf(t);
	switch (kind) {
	case OPT_GAUGES: return &p->gauges;           // 0x83AA64
	case OPT_RACE_POSITION: return &p->racePos;   // 0x83AA65
	case OPT_LAP_INFO: return &p->lapInfo;        // 0x83AA66
	case OPT_SCORE: return &p->score;             // 0x83AA67
	case OPT_FEEDBACK: return &p->feedback;       // 0x83AA68
	case OPT_LEADERBOARD: return &p->leaderboard; // 0x83AA69
	case OPT_ASK_TRANSMISSION: return &p->askTrans; // 0x83AA6A
	default: return NULL;
	}
}

// PORT: the stereo sliders' values, 0..DISPLAY_STEREO_STEPS.
static u32* stereoStepOf(const OptTarget* t, OptKind kind) {
	if (!t->display)
		return NULL;
	switch (kind) {
	case OPT_STEREO_DEPTH: return &t->display->stereoDepth;
	case OPT_STEREO_FOCUS: return &t->display->stereoFocus;
	case OPT_STEREO_HUD: return &t->display->stereoHud;
	default: return NULL;
	}
}

void options_row_value(const OptTarget* t, OptKind kind, OptValue* out) {
	memset(out, 0, sizeof(*out));
	out->number = -1; // only OPT_CONTROL_TYPE prints a number
	// PORT (#447): the button the action is on.
	if (OPT_IS_BIND(kind)) {
		const int s = controls_slot_of((int)(kind - OPT_BIND_FIRST));
		out->text = s >= 0 ? controls_slot_name(s) : "Not Used";   // #527: none
		return;
	}
	// PORT (#448): a Debug Menu row's value, as main.c prints it.
	if (OPT_IS_DBG(kind)) {
		if (s_debugRows && s_debugRows->value)
			out->text = s_debugRows->value((int)(kind - OPT_DBG_FIRST));
		return;
	}
	const OptGameplay* g = &t->opt->gameplay;
	const OptPlayer* p = playerOf(t);

	float* vol = volumeOf(t, kind);
	if (vol) {
		out->fraction = *vol;
		return;
	}
	u8* tog = toggleOf(t, kind);
	if (tog) {
		out->valueHash = *tog ? VAL_ON : VAL_OFF;
		return;
	}
	// The Customize rows. A slider's init (0x49BA70 and the rest) sets min 0,
	// max the value's cap and step 1; a toggle's refresh picks its string
	// with neg/sbb, so any non-zero value reads On, or High.
	const CustomizeRow* cr = customizeRow(kind);
	if (cr && t->display) {
		const u32 v = *display_value(t->display, cr->index);
		if (cr->slider) {
			const u32 cap = display_cap(cr->index);
			out->fraction = cap ? (float)v / (float)cap : 0.0f;
		} else if (cr->highLow) {
			out->valueHash = v ? VAL_HIGH : VAL_LOW;
		} else {
			out->valueHash = v ? VAL_ON : VAL_OFF;
		}
		return;
	}
	u32* step = stereoStepOf(t, kind);
	if (step) {
		out->fraction = (float)*step / (float)DISPLAY_STEREO_STEPS;
		return;
	}

	switch (kind) {
	case OPT_AUDIO_MODE: // 0x49ABD0; neither 0 nor 1 leaves the value unset
		if (t->opt->audio.audioMode == 0)
			out->valueHash = VAL_MONO;
		else if (t->opt->audio.audioMode == 1)
			out->valueHash = VAL_STEREO;
		break;

	case OPT_JUMP_CAMERA: // 0x49ADC0
		out->valueHash = (g->jumpCam == 0.0f) ? VAL_OFF : VAL_ON;
		break;

	case OPT_REARVIEW: // 0x49AE40
		out->valueHash = g->rearview ? VAL_ON : VAL_OFF;
		break;

	case OPT_CAR_DAMAGE: // PORT (#324)
		if (t->display)
			out->valueHash = t->display->b.carDamage ? VAL_ON : VAL_OFF;
		break;

	case OPT_GPS_COLOUR: // PORT (#471)
		out->text = options_gps_colour_name(g->gpsColour);
		break;

	case OPT_SPEEDO_UNITS: // 0x49AEE0
		out->valueHash = g->speedoUnits ? VAL_KMH : VAL_MPH;
		break;

	case OPT_CAREER_DIFFICULTY: { // 0x49AF70; out-of-range words print Easy
		unsigned int v = t->careerSettings ? *t->careerSettings : 0;
		out->valueHash = (v == 1) ? VAL_MEDIUM : (v == 2) ? VAL_HARD : VAL_EASY;
		break;
	}

	case OPT_EXPLORE_MAP: // 0x49B100
	case OPT_RACE_MAP: {  // 0x49B040, the same three strings
		u8 v = (kind == OPT_EXPLORE_MAP) ? g->exploreMap : g->raceMap;
		out->valueHash = (v == 1) ? VAL_STATIC : (v == 2) ? VAL_ROTATE : VAL_OFF;
		break;
	}

	case OPT_SAVE_MODE: // 0x49B210
		if (t->autosaveNoPrompt && *t->autosaveNoPrompt)
			out->valueHash = VAL_SAVE_AUTO;
		else
			out->valueHash = (t->autosave && *t->autosave) ? VAL_SAVE_PROMPT : VAL_SAVE_OFF;
		break;

	case OPT_TRANSMISSION: // 0x4B7630
		if (p->transmission == 0)
			out->valueHash = VAL_AUTO;
		else if (p->transmission == 1)
			out->valueHash = VAL_MANUAL;
		break;

	case OPT_STABILITY: // 0x4B76F0 -- 0 reads On
		if (p->stability == 0)
			out->valueHash = VAL_ON;
		else if (p->stability == 1)
			out->valueHash = VAL_OFF;
		break;

	case OPT_CAMERA: { // 0x4B77B0, jump table 0x4B7848 (five entries)
		static const u32 kCamera[5] = { VAL_BUMPER, VAL_HOOD, VAL_CLOSE, VAL_FAR, VAL_SUPER_FAR };
		if (p->camera <= 4)
			out->valueHash = kCamera[p->camera];
		break;
	}

	case OPT_SPLIT_TIME: { // 0x4B7B00
		static const u32 kSplit[5] = { VAL_LEADER, VAL_LAP_RECORD, VAL_BEST_LAP, VAL_LAST_LAP, VAL_OFF };
		if (p->splitTime <= 4)
			out->valueHash = kSplit[p->splitTime];
		break;
	}

	// COConfig::Draw 0x80308CBC. Retail prints the scheme index with
	// FEPrintf "%d": the stored index plus one, or minus four for a pad with
	// no analogue stick, which is pushed into schemes 5-8. This console
	// always has one, so it is always the first of those.
	case OPT_CONTROL_TYPE:
		out->number = options_control_scheme(t->player) + 1;
		break;

	// COVibration::Draw 0x800EC51C: "On" when the player's vibration byte is
	// set, "Off" otherwise. A 3DS has no rumble, so it is never set.
	case OPT_VIBRATION:
		out->valueHash = VAL_OFF;
		break;

	// 0x49B690 prints a literal per mode of the card ("640x480" for mode 0,
	// up to "1600x1200"); the 3DS's top screen has the one.
	case OPT_RESOLUTION:
		out->text = "400x240";
		break;

	case OPT_LEVEL_OF_DETAIL: // init 0x49B7C0: min 0, max 5.0, step 1.0
		if (t->display)
			out->fraction = (float)t->display->performanceLevel / (float)DISPLAY_LEVEL_MAX;
		break;

	case OPT_STEREO:
		if (t->display)
			out->valueHash = t->display->stereo ? VAL_ON : VAL_OFF;
		break;

	case OPT_STEREO_STYLE:
		if (t->display)
			out->text = t->display->stereoStyle == DISPLAY_STEREO_POP ? "Pop Out" : "Into Screen";
		break;

	// 0x49B8E0 prints On for a set g_VSyncOn; the 3DS is always in step with
	// the display (see buildCustomizeDisplay).
	case OPT_CD_VSYNC:
		out->valueHash = VAL_ON;
		break;

	case OPT_WHEEL_OPTIONS: // a button, no value
	case OPT_CUSTOMIZE_DISPLAY:
	case OPT_DEFAULT_DISPLAY:
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Change handlers -- LEFT 0x9120409E / RIGHT 0xB5971BF1, as in modeopts.c
// ---------------------------------------------------------------------------

// LEFT steps down and wraps to `hi`, RIGHT steps up and wraps to 0: the dec/jns
// and inc/cmp/jle pair every multi-value handler here uses.
static int wrapStep(int v, int hi, bool right) {
	if (!right) {
		v--;
		if (v < 0)
			v = hi;
	} else {
		v++;
		if (v > hi)
			v = 0;
	}
	return v;
}

// 0/1 swap that leaves any other byte alone (0x4B75F7: 0 -> 1, 1 -> 0).
static u8 swap01(u8 v) {
	if (v == 0)
		return 1;
	if (v == 1)
		return 0;
	return v;
}

bool options_adjust(const OptTarget* t, OptKind kind, bool right) {
	OptGameplay* g = &t->opt->gameplay;
	OptPlayer* p = playerOf(t);

	// PORT (#447): the action to the next or previous button.
	if (OPT_IS_BIND(kind)) {
		controls_step((int)(kind - OPT_BIND_FIRST), right);
		return false;
	}
	// PORT (#448): a Debug Menu row steps itself.
	if (OPT_IS_DBG(kind)) {
		if (s_debugRows && s_debugRows->adjust)
			s_debugRows->adjust((int)(kind - OPT_DBG_FIRST), right);
		return false;
	}

	// The slider rows (AddRow 0x54F410; 0x49AB58 sets min 0, max 1.0, step
	// 0x3D4CCCCD): 0.05 a press, clamped to 0..1, then UpdateVolumes 0x469050.
	float* vol = volumeOf(t, kind);
	if (vol) {
		float v = *vol + (right ? 0.05f : -0.05f);
		if (v < 0.0f)
			v = 0.0f;
		if (v > 1.0f)
			v = 1.0f;
		*vol = v;
		return true;
	}
	u8* tog = toggleOf(t, kind);
	if (tog) {
		if (kind == OPT_FEEDBACK) // disabled: retail's row never gets the press
			return false;
		*tog = *tog ? 0 : 1; // test, sete
		return false;
	}
	// The Customize rows. A slider (0x49BA00 and the rest) steps 1.0 inside
	// 0..cap, and only when it moved does the act store it back (ftol); a
	// toggle (0x49BAB0 and the rest) flips the value with xor 1 while its cap
	// is set.
	const CustomizeRow* cr = customizeRow(kind);
	if (cr && t->display) {
		u32* v = display_value(t->display, cr->index);
		const u32 cap = display_cap(cr->index);
		if (cr->slider) {
			if (right && *v < cap)
				(*v)++;
			else if (!right && *v > 0)
				(*v)--;
		} else if (cap) {
			*v ^= 1;
		}
		return false;
	}
	u32* step = stereoStepOf(t, kind);
	if (step) {
		if (right && *step < DISPLAY_STEREO_STEPS)
			(*step)++;
		else if (!right && *step > 0)
			(*step)--;
		return false;
	}

	switch (kind) {
	case OPT_AUDIO_MODE: // 0x49AB80, 0..1 wrapping
		t->opt->audio.audioMode = (u32)wrapStep((int)t->opt->audio.audioMode, 1, right);
		break;

	case OPT_JUMP_CAMERA: // 0x49AD40 -- either way; 0 -> 127.0, anything else -> 0
		g->jumpCam = (g->jumpCam == 0.0f) ? 127.0f : 0.0f;
		break;

	case OPT_REARVIEW: // 0x49AE00 -- test, sete
		g->rearview = g->rearview ? 0 : 1;
		break;

	case OPT_CAR_DAMAGE: // PORT (#324): an On / Off toggle, as Rearview's
		if (t->display)
			t->display->b.carDamage = t->display->b.carDamage ? 0 : 1;
		break;

	case OPT_GPS_COLOUR: // PORT (#471), wrapping
		g->gpsColour = (u8)wrapStep(g->gpsColour < OPTIONS_GPS_COLOURS ? g->gpsColour : 0,
			OPTIONS_GPS_COLOURS - 1, right);
		break;

	case OPT_SPEEDO_UNITS: // 0x49AE80
		g->speedoUnits = swap01(g->speedoUnits);
		break;

	case OPT_CAREER_DIFFICULTY: // 0x49AF20, 0..2 wrapping
		if (t->careerSettings)
			*t->careerSettings = (unsigned int)wrapStep((int)*t->careerSettings, 2, right);
		break;

	case OPT_EXPLORE_MAP: // 0x49B0B0, 0..2 wrapping
		g->exploreMap = (u8)wrapStep(g->exploreMap, 2, right);
		break;

	case OPT_RACE_MAP: // 0x49AFE0 -- split screen tops out at Static
		g->raceMap = (u8)wrapStep(g->raceMap, (t->ctx != CTX_SPLITSCREEN) ? 2 : 1, right);
		break;

	case OPT_SAVE_MODE: { // 0x49B170
		// RIGHT: Autosave -> Prompt -> Off -> Autosave (Prompt for an unnamed
		// profile). LEFT: Autosave -> Off -> Prompt -> Autosave (Off unnamed).
		// 0x502FE0 is UserProfile's "has a name" test.
		if (!t->autosave || !t->autosaveNoPrompt)
			break;
		bool* a = t->autosave;
		bool* np = t->autosaveNoPrompt;
		if (right) {
			if (*np) {
				*a = true; *np = false;        // 0x49B1E1
			} else if (*a) {
				*a = false; *np = false;       // 0x49B1D9
			} else if (t->named) {
				*a = true; *np = true;         // 0x49B1A2
			} else {
				*a = true; *np = false;
			}
		} else {
			if (*np) {
				*a = false; *np = false;
			} else if (!*a) {
				*a = true; *np = false;
			} else if (t->named) {
				*a = true; *np = true;
			} else {
				*a = false; *np = false;
			}
		}
		break;
	}

	case OPT_TRANSMISSION: // 0x4B75D0
		p->transmission = swap01(p->transmission);
		break;

	case OPT_STABILITY: // 0x4B7690
		p->stability = swap01(p->stability);
		break;

	case OPT_CAMERA: // 0x4B7750, 0..3 wrapping (Super Far is never reached)
		p->camera = (u32)wrapStep((int)p->camera, 3, right);
		break;

	case OPT_SPLIT_TIME: // 0x4B7AA0, 0..4 wrapping
		p->splitTime = (u8)wrapStep(p->splitTime, 4, right);
		break;

	// COConfig::Act 0x80308B3C. It wraps between a low and a high it works
	// out from the hardware first: 0 and 3 for a pad with an analogue stick,
	// 5 and 8 for one without, 0 and 1 for a wheel. A 3DS is always the
	// first, so the two ends are constants here.
	case OPT_CONTROL_TYPE: {
		int s = options_control_scheme(t->player);
		if (!right)
			s = (s - 1 < OPTIONS_CONTROL_SCHEME_LO) ? OPTIONS_CONTROL_SCHEME_HI : s - 1;
		else
			s = (s + 1 > OPTIONS_CONTROL_SCHEME_HI) ? OPTIONS_CONTROL_SCHEME_LO : s + 1;
		options_control_set_scheme(t->player, s);
		break;
	}

	// 0x49B740: the slider's level into g_PerformanceLevel, then FUN_005BE9F0
	// sets every value to that level's preset and clamps it. Only when the
	// slider moved, as for the Customize sliders.
	case OPT_LEVEL_OF_DETAIL:
		if (!t->display)
			break;
		if (right && t->display->performanceLevel < DISPLAY_LEVEL_MAX)
			t->display->performanceLevel++;
		else if (!right && t->display->performanceLevel > 0)
			t->display->performanceLevel--;
		else
			break;
		display_apply_level(t->display);
		break;

	case OPT_STEREO:
		if (t->display)
			t->display->stereo = t->display->stereo ? 0 : 1;
		break;

	case OPT_STEREO_STYLE:
		if (t->display)
			t->display->stereoStyle = t->display->stereoStyle == DISPLAY_STEREO_POP
				? DISPLAY_STEREO_INTO : DISPLAY_STEREO_POP;
		break;

	case OPT_WHEEL_OPTIONS: // PORT: a button for a wheel the 3DS cannot have
	default:
		break;
	}
	return false;
}

OptAccept options_accept(const OptTarget* t, OptKind kind) {
	// PORT (#447): A on an action moves it on, as RIGHT does.
	if (OPT_IS_BIND(kind) || OPT_IS_DBG(kind)) {
		options_adjust(t, kind, true);
		return OPTACCEPT_CHANGED;
	}
	// PORT (#527): Delete All Controls does it at once.
	if (kind == OPT_BIND_CLEAR) {
		controls_clear();
		return OPTACCEPT_CHANGED;
	}
	switch (kind) {
	// 0x49B930: FORWARD (0x47FF4E7C) to the screen, whose NotificationMessage
	// pushes UI_PC_Customize_Options.fng on the Display page (0x49C98A).
	case OPT_CUSTOMIZE_DISPLAY:
		return OPTACCEPT_CUSTOMIZE;
	// 0x49B980, on the accept or 0x406415E3: the dim, then the dialog.
	case OPT_DEFAULT_DISPLAY:
	case OPT_BIND_DEFAULT: // PORT (#447): the same question, for the bindings
		return OPTACCEPT_DEFAULT;
	// PORT: the 3-D row takes the accept as the Customize toggles do.
	case OPT_STEREO:
	case OPT_STEREO_STYLE:
	case OPT_GPS_COLOUR: // PORT (#471)
		options_adjust(t, kind, true);
		return OPTACCEPT_CHANGED;
	default:
		break;
	}
	// A Customize toggle's act takes 0x0C407210 as well as LEFT and RIGHT
	// (0x49BAB0's first compare); the sliders hand it to the slider, which
	// does not move on it.
	const CustomizeRow* cr = customizeRow(kind);
	if (cr && !cr->slider && t->display && display_cap(cr->index)) {
		options_adjust(t, kind, true);
		return OPTACCEPT_CHANGED;
	}
	return OPTACCEPT_NONE;
}

// ---------------------------------------------------------------------------
// Page titles -- UI_Options ctor 0x4CF750, table 0x4CFA98
// ---------------------------------------------------------------------------

u32 options_page_title(OptPage page, int ctx) {
	switch (page) {
	case OPTPAGE_AUDIO: return TTL_AUDIO;
	case OPTPAGE_DISPLAY: return TTL_DISPLAY;
	case OPTPAGE_GAMEPLAY: return TTL_GAMEPLAY;
	case OPTPAGE_PLAYER: return TTL_PLAYER;
	case OPTPAGE_EATRAX: return TTL_EATRAX;
	case OPTPAGE_TRAILERS: return TTL_TRAILERS;
	case OPTPAGE_NETWORK: return (ctx == CTX_ONLINE) ? TTL_ONLINE : TTL_LAN;
	case OPTPAGE_CREDITS: return TTL_CREDITS;
	case OPTPAGE_CONTROLS: return TTL_CONTROLS;
	case OPTPAGE_CUSTOMIZE_DISPLAY: return TTL_CUSTOMIZE;
	default: return 0;
	}
}

// ---------------------------------------------------------------------------
// The value-string key table, for callers that resolve English.bin once.

static const u32 kValueHashes[OPTIONS_VALUE_HASH_COUNT] = {
	VAL_ON, VAL_OFF, VAL_MPH, VAL_KMH, VAL_EASY, VAL_MEDIUM, VAL_HARD,
	VAL_STATIC, VAL_ROTATE, VAL_SAVE_AUTO, VAL_SAVE_PROMPT, VAL_SAVE_OFF,
	VAL_MONO, VAL_STEREO, VAL_AUTO, VAL_MANUAL,
	VAL_BUMPER, VAL_HOOD, VAL_CLOSE, VAL_FAR, VAL_SUPER_FAR,
	VAL_LEADER, VAL_LAP_RECORD, VAL_BEST_LAP, VAL_LAST_LAP,
	VAL_HIGH, VAL_LOW,
};

const u32* options_value_hashes(void) {
	return kValueHashes;
}

int options_value_index(u32 hash) {
	for (int i = 0; i < OPTIONS_VALUE_HASH_COUNT; i++) {
		if (kValueHashes[i] == hash)
			return i;
	}
	return -1;
}

// ---------------------------------------------------------------------------
// The Controls screen's button table -- Speed.elf's 0x8035E2C8
//
// Fourteen slots by nine schemes, copied word for word out of the table
// PreFindButtonNameHashForFEString indexes: &0x8035E2C8[slot * 0x24 +
// scheme * 4]. Rows run CButton_1..CButton_14 down the screen's two columns,
// columns run Control Type 1 upwards. No entry in it is zero -- every slot
// does something in every scheme -- but the lookup below still answers 0 for
// one out of range, because that is the answer SetupControllerButtons hides a
// label on.
//
// Reading a column down tells you what that scheme is. Scheme 0 is the
// shipped one: stick to steer, the four free-roam screens on the D-pad, gas
// and brake on two face buttons. Scheme 1 moves the gas to the shoulder at
// slot 1 and the gearshift onto the face buttons; 2 and 3 move it to slot 12
// and the brake to slot 13, two more shoulders. Schemes 5-8 repeat 0-3 with
// the D-pad steering, for a pad with no stick, and this console never
// reaches them (see the header).
// ---------------------------------------------------------------------------

// Short names, so a row of nine fits on one line. Undefined again below.
#define ACT_CHANGE_CAMERA OPT_ACT_CHANGE_CAMERA
#define ACT_ACCELERATE    OPT_ACT_ACCELERATE
#define ACT_BRAKE         OPT_ACT_BRAKE
#define ACT_STEER         OPT_ACT_STEER
#define ACT_STEER_DIGITAL OPT_ACT_STEER_DIGITAL
#define ACT_ENTER_SHOP    OPT_ACT_ENTER_SHOP
#define ACT_WORLD_MAP     OPT_ACT_WORLD_MAP
#define ACT_CAREER_STATS  OPT_ACT_CAREER_STATS
#define ACT_DISPLAY_SMS   OPT_ACT_DISPLAY_SMS
#define ACT_SHIFT_UP      OPT_ACT_SHIFT_UP
#define ACT_SHIFT_DOWN    OPT_ACT_SHIFT_DOWN
#define ACT_HANDBRAKE     OPT_ACT_HANDBRAKE
#define ACT_NITROUS       OPT_ACT_NITROUS
#define ACT_RESET_CAR     OPT_ACT_RESET_CAR
#define ACT_LOOK_BACK     OPT_ACT_LOOK_BACK

static const u32 kControlActions[OPTIONS_CONTROL_BUTTONS][OPTIONS_CONTROL_SCHEMES] = {
	{ ACT_CHANGE_CAMERA, ACT_ACCELERATE,    ACT_CHANGE_CAMERA, ACT_CHANGE_CAMERA, ACT_CHANGE_CAMERA, ACT_CHANGE_CAMERA, ACT_ACCELERATE,    ACT_CHANGE_CAMERA, ACT_CHANGE_CAMERA },
	{ ACT_BRAKE,         ACT_SHIFT_DOWN,    ACT_SHIFT_DOWN,    ACT_NITROUS,       ACT_SHIFT_DOWN,    ACT_BRAKE,         ACT_SHIFT_DOWN,    ACT_SHIFT_DOWN,    ACT_NITROUS },
	{ ACT_STEER,         ACT_STEER,         ACT_STEER,         ACT_STEER,         ACT_STEER,         ACT_ENTER_SHOP,    ACT_ENTER_SHOP,    ACT_ENTER_SHOP,    ACT_ENTER_SHOP },
	{ ACT_ENTER_SHOP,    ACT_ENTER_SHOP,    ACT_ENTER_SHOP,    ACT_ENTER_SHOP,    ACT_ENTER_SHOP,    ACT_WORLD_MAP,     ACT_WORLD_MAP,     ACT_WORLD_MAP,     ACT_WORLD_MAP },
	{ ACT_WORLD_MAP,     ACT_WORLD_MAP,     ACT_WORLD_MAP,     ACT_WORLD_MAP,     ACT_WORLD_MAP,     ACT_CAREER_STATS,  ACT_CAREER_STATS,  ACT_CAREER_STATS,  ACT_CAREER_STATS },
	{ ACT_CAREER_STATS,  ACT_CAREER_STATS,  ACT_CAREER_STATS,  ACT_CAREER_STATS,  ACT_CAREER_STATS,  ACT_DISPLAY_SMS,   ACT_DISPLAY_SMS,   ACT_DISPLAY_SMS,   ACT_DISPLAY_SMS },
	{ ACT_DISPLAY_SMS,   ACT_DISPLAY_SMS,   ACT_DISPLAY_SMS,   ACT_DISPLAY_SMS,   ACT_DISPLAY_SMS,   ACT_STEER_DIGITAL, ACT_STEER_DIGITAL, ACT_STEER_DIGITAL, ACT_STEER_DIGITAL },
	{ ACT_SHIFT_UP,      ACT_RESET_CAR,     ACT_RESET_CAR,     ACT_SHIFT_UP,      ACT_NITROUS,       ACT_SHIFT_UP,      ACT_RESET_CAR,     ACT_RESET_CAR,     ACT_SHIFT_UP },
	{ ACT_ACCELERATE,    ACT_SHIFT_UP,      ACT_SHIFT_UP,      ACT_HANDBRAKE,     ACT_SHIFT_UP,      ACT_ACCELERATE,    ACT_SHIFT_UP,      ACT_SHIFT_UP,      ACT_HANDBRAKE },
	{ ACT_RESET_CAR,     ACT_HANDBRAKE,     ACT_NITROUS,       ACT_RESET_CAR,     ACT_LOOK_BACK,     ACT_RESET_CAR,     ACT_HANDBRAKE,     ACT_NITROUS,       ACT_RESET_CAR },
	{ ACT_SHIFT_DOWN,    ACT_CHANGE_CAMERA, ACT_HANDBRAKE,     ACT_SHIFT_DOWN,    ACT_HANDBRAKE,     ACT_SHIFT_DOWN,    ACT_CHANGE_CAMERA, ACT_HANDBRAKE,     ACT_SHIFT_DOWN },
	{ ACT_HANDBRAKE,     ACT_LOOK_BACK,     ACT_ACCELERATE,    ACT_ACCELERATE,    ACT_ACCELERATE,    ACT_HANDBRAKE,     ACT_LOOK_BACK,     ACT_ACCELERATE,    ACT_ACCELERATE },
	{ ACT_NITROUS,       ACT_NITROUS,       ACT_BRAKE,         ACT_BRAKE,         ACT_BRAKE,         ACT_NITROUS,       ACT_NITROUS,       ACT_BRAKE,         ACT_BRAKE },
	{ ACT_LOOK_BACK,     ACT_BRAKE,         ACT_LOOK_BACK,     ACT_LOOK_BACK,     ACT_LOOK_BACK,     ACT_LOOK_BACK,     ACT_BRAKE,         ACT_LOOK_BACK,     ACT_LOOK_BACK },
};

#undef ACT_CHANGE_CAMERA
#undef ACT_ACCELERATE
#undef ACT_BRAKE
#undef ACT_STEER
#undef ACT_STEER_DIGITAL
#undef ACT_ENTER_SHOP
#undef ACT_WORLD_MAP
#undef ACT_CAREER_STATS
#undef ACT_DISPLAY_SMS
#undef ACT_SHIFT_UP
#undef ACT_SHIFT_DOWN
#undef ACT_HANDBRAKE
#undef ACT_NITROUS
#undef ACT_RESET_CAR
#undef ACT_LOOK_BACK

// FEHashUpper("CButton_1") .. ("CButton_14"), the names SetupControllerButtons
// builds with FEngSNPrintf(0x803242F0, i) and looks up one at a time.
static const u32 kControlObjects[OPTIONS_CONTROL_BUTTONS] = {
	0xBAA4198Eu, 0xBAA4198Fu, 0xBAA41990u, 0xBAA41991u, 0xBAA41992u,
	0xBAA41993u, 0xBAA41994u, 0xBAA41995u, 0xBAA41996u, 0x0F274B7Eu,
	0x0F274B7Fu, 0x0F274B80u, 0x0F274B81u, 0x0F274B82u,
};

u32 options_control_object(int slot) {
	if (slot < 0 || slot >= OPTIONS_CONTROL_BUTTONS)
		return 0;
	return kControlObjects[slot];
}

u32 options_control_action(int slot, int scheme, bool freeRoam) {
	if (slot < 0 || slot >= OPTIONS_CONTROL_BUTTONS ||
			scheme < 0 || scheme >= OPTIONS_CONTROL_SCHEMES)
		return 0;
	const u32 a = kControlActions[slot][scheme];
	// FindButtonNameHashForFEString 0x80152C8C: the four screens free roam
	// owns have no button outside it, and the label says so rather than
	// going blank.
	if (!freeRoam && (a == OPT_ACT_DISPLAY_SMS || a == OPT_ACT_WORLD_MAP ||
			a == OPT_ACT_CAREER_STATS || a == OPT_ACT_ENTER_SHOP))
		return OPTIONS_NOT_USED_HASH;
	return a;
}

// FEDatabase+0xBC. Not in OptPlayer: it is not one of the settings blocks the
// save carries, so it starts at 0 every run, as it does on the GameCube.
static int s_scheme[OPTIONS_PLAYERS];

int options_control_scheme(int player) {
	if (player < 0 || player >= OPTIONS_PLAYERS)
		return OPTIONS_CONTROL_SCHEME_LO;
	return s_scheme[player];
}

void options_control_set_scheme(int player, int scheme) {
	if (player < 0 || player >= OPTIONS_PLAYERS)
		return;
	if (scheme < OPTIONS_CONTROL_SCHEME_LO)
		scheme = OPTIONS_CONTROL_SCHEME_LO;
	if (scheme > OPTIONS_CONTROL_SCHEME_HI)
		scheme = OPTIONS_CONTROL_SCHEME_HI;
	s_scheme[player] = scheme;
}
