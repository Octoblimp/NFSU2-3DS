#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbglog.h"
#include "fng.h"
#include "presetcar.h"
#include "c2dtext.h"
#include "langbin.h"
#include "race.h"

// ---- HUD.nfsh (tools/hud_pack.py's pack_hud() has the layout) -------------
#define NFSH_MAGIC     0x4853464Eu // 'NFSH' little-endian
#define NFSH_VERSION   2
#define NFSH_HDR       0x20
#define NFSH_REC       16
#define NFSH_STYLE_REC 8
#define NFSH_FMT_RGB565 0
#define NFSH_FMT_RGBA4  1

// ---- canvas -> top screen ---------------------------------------------------
// Half scale, the canvas's right and bottom edges on the screen's: canvas
// (320, 240) lands on (400, 240). See hud.h on why that corner.
#define HUD_SCALE    0.5f
#define HUD_ORIGIN_X (400.0f - 320.0f * HUD_SCALE)
#define HUD_ORIGIN_Y (240.0f - 240.0f * HUD_SCALE)
// isCentred()'s widgets: canvas x = 0 on the top screen's middle.
#define HUD_CENTRE_X 200.0f
// isGetawayMeter()'s: the canvas's left edge on the screen's, so canvas
// x = 0 lands 160 px in (engage.c's egScreenX).
#define HUD_LEFT_X   (320.0f * HUD_SCALE)

// The unit label is authored at 14pt x 0.75, which is 5 px at this scale.
// Lifted to a legible floor exactly as the menus' MENU_MIN_TEXT_PX does, and
// for the same reason: a port accommodation for a quarter of the canvas's
// pixels, not engine behaviour. Strings already above it are untouched.
#define HUD_MIN_TEXT_PX 9.0f

// PORT (#334, the user 2026-11-02: "Make the green text such as 'Bank reward
// for magazine' or wtv bigger since the 3DS has a smaller screen. But still
// keep it centered, and don't make it too big."): the HUD's green pop-up
// notices (all three are 202,244,138 in HUD_SingleRace.fng), each drawn this
// much over retail's size at the cluster's half scale, about its own authored
// anchor so a centred one stays centred. Retail authors them at different
// sizes -- the reward line ends at 0.65 of its widget, the Outrun line at
// 0.82, the style moment at its full size -- so each has its own factor,
// chosen on Azahar to read at 400 x 240 without crowding the street.
#define HUD_NOTICE_CASH   2.0f   // Cash_Reward, "Bank Reward for Magazine: N"
#define HUD_NOTICE_OUTRUN 1.2f   // Out_Run_Message, "You have the Lead!"
#define HUD_NOTICE_STUNT  1.3f   // CunningStuntSpread's name and points
#define HUD_NOTICE_SPLIT  1.6f   // SplitTimeText, "-1.94" (#502; #334's green lines)
// DbgHudNoticeScale=<f> blends them for an A/B: 0 = retail's sizes, 1 = the
// port's (the default).
float g_hudNoticeScale = 1.0f;
#define HUD_NOTICE(k) (1.0f + ((k) - 1.0f) * g_hudNoticeScale)

// Tachometer::Update's speed factors (0x80326338 / 0x8032633C), m/s to the
// displayed unit, picked by Player::IsHudKPH (0x80215798).
#define HUD_MS_TO_KPH 3.6f
#define HUD_MS_TO_MPH 2.237f

// The ctor's sweep constant (0x8032630C): the plate's printed scale covers
// 235 degrees, so a car's needle travels limiter / plate-top of that.
#define HUD_PLATE_SWEEP_DEG 235.0f

// Tachometer::Update's gear colours, 0xAARRGGBB: grey with the clutch home
// (CarState+0x418, DriveTrain+0x44 == 3), orange while it is out mid-shift.
// PerfectShiftSplit, the share of the perfect-shift range before the optimal
// revs where Tachometer::Update lights Shift_light (#500).
#define HUD_PERFECT_SHIFT_SPLIT 0.95f
#define HUD_GEAR_ENGAGED 0xFFC3C3C3u
#define HUD_GEAR_SHIFTING 0xFFFF8040u

// TurboMeter::CalcNeedleAngle (0x80109AC8) with Update's one-screen pair
// (0x8032637C / 0x80326380): boost -1..1 across -14..222 degrees, past 360
// taken back a turn, and a negative angle as 360 less it.
static float turboNeedleAngle(float boost) {
	const float a0 = -14.0f, a1 = 222.0f;
	float v = (a1 - a0) * ((boost + 1.0f) * 0.5f) + a0;
	if (360.0f < v)
		v -= 360.0f;
	return 0.0f <= v ? v : 360.0f - v;
}

// NitrousGauge::Update's one-player path (Speed.elf 0x8010D324, SPEED2.EXE
// 0x4C7B20): NOS_Mask turns to -28 - (152 - 152 * nos) degrees (0x80326744,
// 0x80326740), so a full bottle leaves 28 degrees of its half disc past the
// arc's N2O end and an empty one has turned it off the arc entirely.
#define HUD_NOS_MASK_SWEEP 152.0f
#define HUD_NOS_MASK_FULL  (-28.0f)
// NOSMaskClip (0x8046E9D4 = 0xAA000080): psNOSGaugeAlphaMask's TEV keeps a
// gamebreaker texel where TACH_NOS's red is above this.
#define HUD_NOS_CLIP 0xAA
// The gamebreaker's colours, FEngSetColor from the constructor's +0x2C (a
// bottle up to full) and +0x30 (the overfill): SPEED2.EXE 0x4F6BCF / 0x4F6BD6.
// The GameCube reads the same pair from TweakRegulalNOSColour.
#define HUD_NOS_COLOUR      0x96005A7Du
#define HUD_NOS_COLOUR_OVER 0x9600B4FAu

typedef enum {
	// Images first: their index is also their texture slot.
	HE_FILL, HE_REDLINE, HE_PLATE, HE_NEEDLE,
	HE_NOS_FILL, HE_NOS_LINE, HE_NOS_GB, HE_NOS_MASK, HE_SHIFT,
	// TurboMeter's six (__10TurboMeterPCci 0x801098B0), up only for a car
	// with a turbo (SetTurboVisibility); the dial is its needle.
	HE_TURBO_FILL, HE_TURBO_LINES, HE_TURBO_DIAL, HE_TURBO_RED1, HE_TURBO_RED2,
	HE_TURBO_BACKING,
	HE_LAP_BACKING, HE_TIME_BACKING,
	// WrongWIndi's image, driven by its own scripts rather than by isRaceElem.
	HE_WRONG_WAY,
	// GetAwayMeter's three (#268 H step 2), drawn while HudFrame.ga.on.
	HE_GA_BACKING, HE_GA_LINES, HE_GA_BAR,
	HE_IMAGES,
	HE_SPEED = HE_IMAGES, HE_UNITS, HE_GEAR,
	HE_LAP_TEXT, HE_LAP_NUMBER, HE_NUM_LAPS, HE_TIME_TEXT, HE_CUR_TIME,
	HE_LAP_TITLE, HE_LAP_TITLE_TIME, HE_COUNTDOWN, HE_RACE_OVER,
	// An Image, but its art is White16x16 tinted: drawn as a solid rectangle.
	HE_RACE_OVER_BACKING,
	// CunningStuntSpread's two strings, free roam and races alike.
	HE_SP_TYPE, HE_SP_POINT,
	// ShowcaseRewardIndicator's one string, a photo shoot's pay (#268 B).
	HE_CASH_REWARD,
	// GetAwayMeter's strings: the lead, its unit and Out_Run_Message.
	HE_GA_NUMBER, HE_GA_TEXT, HE_OUTRUN_MSG,
	// SplitTime's string (#502, __9SplitTimePCci 0x8010A62C).
	HE_SPLIT_TEXT,
	HE_COUNT
} HudElem;

// The race widgets, drawn only while a race is on.
static bool isRaceElem(int e) {
	return e == HE_LAP_BACKING || e == HE_TIME_BACKING ||
		(e >= HE_LAP_TEXT && e <= HE_RACE_OVER_BACKING);
}

// CunningStuntSpread's (feature mask 0x20000, the Player page's Score row).
static bool isStuntElem(int e) {
	return e == HE_SP_TYPE || e == HE_SP_POINT;
}

// LapCounter's widgets (feature mask 0x508): the two backings and every
// string from the lap label to the lap title's time.
static bool isLapElem(int e) {
	return e == HE_LAP_BACKING || e == HE_TIME_BACKING ||
		(e >= HE_LAP_TEXT && e <= HE_LAP_TITLE_TIME);
}

// The Tachometer's and TurboMeter's widgets (feature mask 0x10000060) and
// NitrousGauge's (0x00040000 in the mask's high word, which DetermineGauges
// sets only on its Gauges-on path).
static bool isGaugeElem(int e) {
	return e < HE_LAP_BACKING || (e >= HE_SPEED && e <= HE_GEAR);
}

// NitrousGauge's four. DetermineGauges (0x802160E8) adds their 0x40000 only
// while the car's bottle (CarState +0x424) holds any nitrous, so a car with
// none fitted shows no N2O arc at all, backing included.
static bool isNosElem(int e) {
	return e >= HE_NOS_FILL && e <= HE_NOS_MASK;
}

static bool isTurboElem(int e) {
	return e >= HE_TURBO_FILL && e <= HE_TURBO_BACKING;
}

// The ones retail authors at the canvas's centre line. The cluster's corner
// mapping (hud.h) would push them 40 px right of the screen's middle, so
// these map canvas x = 0 to the top screen's centre instead -- the same
// half scale, a port accommodation for the narrower screen.
static bool isCentred(int e) {
	return e == HE_LAP_TITLE || e == HE_LAP_TITLE_TIME || e == HE_COUNTDOWN ||
		e == HE_RACE_OVER || e == HE_RACE_OVER_BACKING || e == HE_WRONG_WAY ||
		e == HE_CASH_REWARD || e == HE_OUTRUN_MSG || e == HE_SPLIT_TEXT;
}

// GetAwayMeter's, authored on the canvas's top-left: mapped from that corner,
// as the Engage widget is (engage.c), so the meter sits where retail has it.
static bool isGetawayMeter(int e) {
	return e == HE_GA_BACKING || e == HE_GA_LINES || e == HE_GA_BAR ||
		e == HE_GA_NUMBER || e == HE_GA_TEXT;
}

// The names the constructors hand FEHashUpper, which is the uppercase-folding
// bStringHash presetcar_hash() already implements -- the .fng's object hashes
// match them exactly. NULL where the constructor registers a literal hash.
static const char* const kWidgetName[HE_COUNT] = {
	"tach_fill",               // Tachometer +0x80
	"Redline",                 // +0x7C
	"3rdPersonTachRPMNumbers", // +0x50
	"3rdPersonNeedle",         // +0x30
	"NOS_Fill",                // NitrousGauge (__12NitrousGaugePCci 0x8010D210)
	"NOS_Line",                // +0x20
	"NOS_GameBreaker",         // +0x24
	"NOS_Mask",                // +0x28
	"Shift_light",             // Tachometer +0x78
	"3rdperson_Turbofill",     // TurboMeter +0x20 (Tachometer +0x88)
	"3rdperson_Turbolines",    // +0x24 (Tachometer +0x84)
	"3rdperson_TurboDial",     // +0x28 (Tachometer +0x8C)
	"3rdperson_TurboRed_1",    // +0x2C
	"3rdperson_TurboRed_2",    // +0x30
	"Drift_Angle_Backing2",    // +0x34
	"LapCounterBacking_1",     // LapCounter +0x40 (__10LapCounterPCci 0x80109BA8)
	"LapCounterBacking_2",     // +0x44
	"WrongWayImage",           // WrongWIndi +0x20 (__10WrongWIndiPCci 0x8010BF08)
	"Getaway_Distance_Backing", // GetAwayMeter (__12GetAwayMeterPCci 0x801123E8)
	"Getaway_Distance_Lines",
	"Getaway_Distance_Bar",    // +0x28
	"3rdPersonSpeed",         // +0x58
	"3rdPersonSpeedUnits",     // +0x5C
	"3rdPersonGear",           // +0x64
	"LapText",                 // LapCounter +0x30
	"LapNumber",               // +0x20
	"NumberLaps",              // +0x24
	"CurrentLapTimeText",      // registered as a plain object
	"CurrentLapTime",          // +0x2C
	"Lap_Time_Title",          // +0x34
	"Lap_Time_Text",           // +0x38
	NULL,                      // Countdown +0x20 (__9CountdownPCci 0x8010E7E4)
	"RaceOverMessage",         // RaceOverMessage +0x20 (0x8010DF04)
	"RaceOverBacking",         // +0x24
	"SP_Trick_Type",           // CunningStuntSpread +0x38 (0x8010F1E0)
	"SP_Trick_Point",          // +0x34
	"Cash_Reward",             // ShowcaseRewardIndicator +0x20 (0x80108B1C)
	"Distance_Number",         // +0x2C
	"Distance_Text",           // +0x34
	"Out_Run_Message",         // +0x30
	"SplitTimeText",           // SplitTime +0x20 (its SplitTimeLapTime, +0x24, is
	                           // only ever hidden by Update)
};
#define HUD_COUNTDOWN_HASH 0x2DA220AAu

// ---- CunningStuntSpread (TASKS.md #311) -------------------------------------
// The constructor's colours (+0x60 / +0x68), 0xAARRGGBB: a moment's points
// and name in green, an aborted or negative one in red.
#define HUD_SP_GREEN 0xFFCAF48Au
#define HUD_SP_RED   0xFFCF3030u
// DrawStunt shows this moment's points whole, never divided by its multiplier.
#define HUD_SP_WHOLE_HASH 0xA46AAEA8u
// UpdateStunts: a drawn moment comes off the list this long after it was last
// drawn or updated (0x80326E00), or sooner with others queued (0x80326DFC).
#define HUD_SP_HOLD_ONE  1.0f
#define HUD_SP_HOLD_MANY 0.5f
// PORT: retail's list is a bList with no bound; a moment type has one node at
// a time and each goes within a second of its last update, so this is never
// reached on retail's moments. A new one past it is dropped.
#define HUD_SP_MAX 16
// The moment state DrawStunt parks a node in once its fadeout has started.
#define HUD_SP_STATE_FADING 5

// The scripts DrawStunt plays on the two strings, by FEngSetScript name.
enum { SP_SCRIPT_NONE, SP_SCRIPT_ANIMATE, SP_SCRIPT_FADEOUT, SP_SCRIPT_FADEOUT_CRASH };

// One CunningStunt (0x2C bytes; the comments are its word index).
typedef struct {
	bool continuous;   // [2], the moment record's +0x06
	float points;      // [3]
	int mult;          // [4], (int)(multiplier + 0.5)
	int type;          // [5], eStyleMomentType
	s32 shownAt;       // [6], WorldTimer at its last draw; 0 = not since its update
	u32 nameHash;      // [7], the record's English.bin key
	int drawn;         // [8]
	int state;         // [9], the moment's state, or HUD_SP_STATE_FADING
	bool aborted;      // [10]
	char name[24];     // [7] through English.bin (style_load's lookup)
} HudStunt;

// What DrawStunt and FEngSetScript leave on one of the two strings.
typedef struct {
	bool visible;
	int script;        // SP_SCRIPT_*
	s32 startedAt;     // WorldTimer when it started
	u32 color;         // FEngSetColor's, 0xAARRGGBB
	char text[48];
} HudStuntStr;

// The script WrongWayImage is running (see wrongWayUpdate).
enum { WW_IDLE, WW_FADE_IN, WW_LOOP, WW_FADE_OUT };

typedef struct {
	bool found;
	float x, y;      // canvas centre, summed up the parent chain
	float depth;
	float w, h;      // Image: canvas size. String: the SA scale.
	u32 color;       // citro2d colour
	u8 rgba[4];      // the same, for the widgets that animate alpha
	u16 fontPt;
	u32 justify;
	u32 resource;    // the .fng's own texture hash, 0 if none
	char text[40];   // a String's own text: its SH through English.bin, else
	                 // the literal the .fng carries
} HudWidget;

// The race stats' widgets out of HUD_SingleRace.fng (hud_draw_race_stats):
// RacePosition's backing and three strings, then LeaderBoard's four rows of
// LBData_%d, LBTextBacking_%d, LeaderText_%d and LBBacking_%d (+0x170 = 4).
#define HUD_LB_ROWS 4
typedef enum {
	SE_BACKING,     // PositionBacking, 3RDPERSON_BOXCORNER
	SE_NUMBER,      // RacePositionNumber +0x28
	SE_NTH,         // RacePositionNth +0x2C
	SE_RACERS,      // NumberRacers +0x30
	SE_LB_DATA,     // +0x174
	SE_LB_TEXT_BACK = SE_LB_DATA + HUD_LB_ROWS,     // +0x194
	SE_LEADER_TEXT = SE_LB_TEXT_BACK + HUD_LB_ROWS, // +0x184
	SE_LB_BACK = SE_LEADER_TEXT + HUD_LB_ROWS,      // +0x1A4
	SE_COUNT = SE_LB_BACK + HUD_LB_ROWS
} HudStatElem;

struct Hud {
	HudWidget w[HE_COUNT];
	int order[HE_COUNT];
	int orderCount;

	C3D_Tex tex[HE_IMAGES];
	Tex3DS_SubTexture sub[HE_IMAGES];
	bool texValid[HE_IMAGES];

	// The rear-view mirror's RVM and RVM_MASK (#501, hud_rvm_art()).
	C3D_Tex rvmTex[2];
	bool rvmValid[2];

	// The gamebreaker as psNOSGaugeAlphaMask draws it: TACH_NOS clipped at
	// NOSMaskClip, inside the turned NOS_Mask. Both kept on the CPU at load;
	// the A8 result is rebuilt when the mask's angle moves, into the texture
	// the GPU is not reading this frame.
	u8* nosShape;      // NOS_GameBreaker's texels, 1 where red > NOSMaskClip
	u8* nosMaskA;      // NOS_Mask's alpha, row 0 at the top
	u16 nosShapeW, nosShapeH, nosMaskW, nosMaskH;
	C3D_Tex nosTex[2];
	bool nosTexValid;
	int nosCur;
	float nosAngle;    // the angle nosTex[nosCur] was built for

	C2D_TextBuf buf;
	float sweep;
	float limiter;
	bool kph;
	char unitText[16];

	// LapCounter's own state (+0x54..+0x68), reset per race.
	s32 lcNewLapAt;   // +0x54, when the last lap ended
	s32 lcShown;      // +0x58, the time CurrentLapTime shows
	s32 lcTitleAt;    // +0x5C, when Lap_Time_Title went up
	bool lcBest;      // +0x64, that lap was the best so far
	bool lcHadLap;    // +0x68, a lap has ended this race
	// SplitTime's (#502): +0x28 ZoomIn ran and the string is up, +0x2C when
	// (Race::now), +0x34 the time shown and its text; green for <= 0.
	bool stZoomed;
	s32  stZoomAt;
	s32  stSplit;
	bool stGreen;
	char stText[24];
	// RaceOverMessage +0x2C/+0x28: when the message's script started.
	s32 roStartedAt;
	bool roStarted;
	// WrongWIndi +0x28, "FadeIn has been set", and the script the image is
	// running with the wall-clock ms it started at.
	bool wwShown;
	int wwScript;
	u64 wwStartMs;

	// CunningStuntSpread's list (+0x20) and its two strings (+0x38 the name,
	// +0x34 the points). `spLastNow` catches a WorldTimer that went back.
	HudStunt sp[HUD_SP_MAX];
	int spCount;
	HudStuntStr spName, spPoints;
	s32 spLastNow;
	void (*log)(const char*);

	// English.bin strings the race widgets set by language hash.
	char sGo[16];          // 0x00000535
	char sCompleted[32];   // 0xB1EE4CA9
	char sNoTime[16];      // 0x61C9B7A2
	char sLapTime[32];     // 0xFC1B498A
	char sBestLapTime[32]; // 0xA8247197
	char sYourTime[32];    // 0xC2878EBC
	char sYourScore[32];   // 0x13640DE9, a drift's
	char sBlown[32];       // 0x7449D26D
	char sTotaled[32];     // 0x4BA0D22F

	// The drag race's package while one is open (hud_drag_open), else NULL.
	struct HudDrag* drag;
	// The drift race's (hud_drift_open), else NULL.
	struct HudDrift* driftPkg;

	// ShowcaseRewardIndicator's queue (its statics, SpeedPS.elf 0x8046DB8C..
	// 0x8046DB9C): the line QueueMessage asked for, the frames still to wait,
	// and the ZoomIn that Update starts once they are gone.
	char crText[64];
	bool crQueued;
	int crDelay;
	bool crShown;
	u64 crStartMs;
	char sMagReward[48];   // 0xABA1E56F
	char sDvdReward[48];   // 0x2AB10D61

	// GetAwayMeter (#268 H step 2): Show's counter (+0x44), the lead as the
	// last Update left it (+0x24), the pulse Distance_Number runs (1 Ahead,
	// 2 Behind, 0 none) and when it started, and Out_Run_Message's text and
	// its ZoomIn's start.
	int  gaShow;
	bool gaActive;         // the group's "Active" script ran, "InActive" since not
	float gaPrev;
	int  gaPulse;
	u64  gaPulseMs;
	char orText[72];
	bool orShown;
	u64  orStartMs;
	char sMeters[16];      // 0x6BB2D6AF
	char sFeet[16];        // 0x001578C3
	char sLeadGot[40];     // 0xBFD1936F
	char sLeadLost[40];    // 0x9B2AF3C7
	char sAcceptLose[72];  // 0x783202B0
	char sAcceptCatch[72]; // 0x18B97380
	char sWonOutrun[48];   // 0xDC089C64
	char sLostOutrun[48];  // 0x5D1621B2
	char sBankReward[32];  // 0x5D10F6AF

	// The race stats (hud_draw_race_stats), with their own text buffer: the
	// bottom screen draws them on frames the cluster is not up.
	HudWidget st[SE_COUNT];
	C3D_Tex stTex;
	Tex3DS_SubTexture stSub;
	bool stTexValid;
	C2D_TextBuf stBuf;
	// RacePosition +0x4C, when the player's car last began a lap, and +0x50,
	// the race time RaceTime shows (held for 3 s past each new lap).
	s32 rpNewLapAt;
	s32 rpTime;
	// LeaderBoard +0x1BC, when the rows last switched, and +0x1C0, names up
	// (else the gaps).
	s32 lbSince;
	bool lbNames;
	// GetLocalizedOrdinal's: OrdinalHashes (0x8035BCDC) th st nd rd, and
	// 0xBCDD9448 for 10.
	char sOrd[4][8];
	char sOrd10[8];
	// DrawTimes' finished cars, by finish reason.
	char sFinished[24];     // 0xD4293449
	char sKnockedOut[24];   // 0x5D82DBA2, reason 3
	char sBlownEngine[24];  // 0x01E66364, 8
	char sTotaledLb[24];    // 0xB7B75185, 9
	char sTimeup[24];       // 0x1CE129E0, 0xB
	char sDisconnected[24]; // 0xA7DB717F, 0xC
};

static u32 rd32(const u8* b) {
	return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

static u16 rd16(const u8* b) {
	return (u16)(b[0] | (b[1] << 8));
}

static u32 argbToC2d(u32 argb) {
	return C2D_Color32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, argb >> 24);
}

// A HUD part's tint, as the constructor builds it: RED/GREEN/BLUE at alpha
// 255, or white for an empty slot.
static u32 tintToC2d(u32 rgb) {
	if (rgb == 0xFFFFFFFFu)
		return C2D_Color32(255, 255, 255, 255);
	return C2D_Color32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

// GetTachStyle (0x80241F3C), on EngineInfo's redline (+4) and limiter (+8).
static int tachStyle(float redline, float limiter) {
	if (limiter >= 9000.0f && redline >= 8500.0f)
		return redline < 9000.0f ? 1 : 0;
	if (limiter >= 8000.0f && redline >= 7500.0f)
		return redline < 8000.0f ? 3 : 2;
	if (limiter >= 7000.0f && redline >= 6500.0f)
		return redline < 7000.0f ? 5 : 4;
	return redline < 6000.0f ? 7 : 6;
}

// CalcAngleForRPM (0x801094B0), constants 0x80326324/28/2C = 0, 1, 360.
static float calcAngleForRpm(float rpm, float sweep, float limiter) {
	float t = rpm / limiter;
	if (!(t >= 0.0f))
		t = 0.0f;
	else if (t > 1.0f)
		t = 1.0f;
	const float s = sweep <= 0.0f ? sweep + 360.0f : sweep;
	const float a = t * s;
	if (a <= 360.0f)
		return a < 0.0f ? 360.0f - a : a;
	return a - 360.0f;
}

// GearToLetter, on the port's numbering (-1 R, 0 N, 1..n). The engine's own
// enum is one higher -- 0 R, 1 N, 2 first -- and its table runs to '7'.
static char gearLetter(int gear) {
	if (gear < 0)
		return 'R';
	if (gear == 0)
		return 'N';
	return gear <= 9 ? (char)('0' + gear) : '9';
}

// Texel (x, y) of a w-wide texture in car_pack.encode_texture()'s layout: 8x8
// tiles in rows, Morton order inside a tile (x on the even bits), row 0 the
// art's top row.
static u32 tiledIndex(u32 x, u32 y, u32 w) {
	u32 m = 0;
	for (int b = 0; b < 3; b++)
		m |= ((x >> b) & 1u) << (2 * b) | ((y >> b) & 1u) << (2 * b + 1);
	return ((y >> 3) * (w >> 3) + (x >> 3)) * 64u + m;
}

// What the gamebreaker composite needs of TACH_NOS and NOS_MASK, unswizzled:
// the arc where its red is above NOSMaskClip, and the mask's alpha.
static void keepNosTexels(Hud* h, int e, const u8* px, u16 w, u16 ht, u16 fmt) {
	u8* out = (u8*)malloc((size_t)w * ht);
	if (!out)
		return;
	for (u32 y = 0; y < ht; y++) {
		for (u32 x = 0; x < w; x++) {
			const u32 i = tiledIndex(x, y, w) * 2;
			const u16 t = (u16)(px[i] | (px[i + 1] << 8));
			u8 v;
			if (e == HE_NOS_GB) {
				const u32 r = fmt == NFSH_FMT_RGB565 ? ((t >> 11) << 3) | (t >> 13)
				                                     : (t >> 12) * 17u;
				v = r > HUD_NOS_CLIP;
			} else {
				v = fmt == NFSH_FMT_RGB565 ? 255 : (u8)((t & 0xF) * 17u);
			}
			out[y * w + x] = v;
		}
	}
	if (e == HE_NOS_GB) {
		free(h->nosShape);
		h->nosShape = out;
		h->nosShapeW = w;
		h->nosShapeH = ht;
	} else {
		free(h->nosMaskA);
		h->nosMaskA = out;
		h->nosMaskW = w;
		h->nosMaskH = ht;
	}
}

// An open HUD.nfsh: the file, its record table and the style table after it.
typedef struct {
	FILE* f;
	u8* table;
	u32 count, blobOff, blobSize, nStyles;
} Nfsh;

static void nfshClose(Nfsh* n) {
	free(n->table);
	if (n->f)
		fclose(n->f);
	memset(n, 0, sizeof(*n));
}

// False, having logged why, when the file is missing, not this version, or
// its tables do not read.
static bool nfshOpen(Nfsh* n, const char* path, void (*log)(const char*)) {
	char b[256];
	memset(n, 0, sizeof(*n));
	n->f = fopen(path, "rb");
	u8 hdr[NFSH_HDR];
	if (!n->f || fread(hdr, 1, NFSH_HDR, n->f) != NFSH_HDR || rd32(hdr) != NFSH_MAGIC ||
			rd32(hdr + 4) != NFSH_VERSION) {
		snprintf(b, sizeof(b), "hud: %s missing or not a v%d HUD.nfsh -- no HUD "
			"(run tools/hud_pack.py)", path, NFSH_VERSION);
		log(b);
		nfshClose(n);
		return false;
	}
	n->count = rd32(hdr + 8);
	n->blobOff = rd32(hdr + 0xC);
	n->blobSize = rd32(hdr + 0x10);
	n->nStyles = rd32(hdr + 0x14);
	const u32 styleOff = rd32(hdr + 0x18);
	const u32 tableBytes = n->count * NFSH_REC + n->nStyles * NFSH_STYLE_REC;
	n->table = (n->count && n->count < 4096 && n->nStyles < 4096) ? (u8*)malloc(tableBytes) : NULL;
	if (!n->table || fread(n->table, 1, tableBytes, n->f) != tableBytes ||
			styleOff != NFSH_HDR + n->count * NFSH_REC) {
		log("hud: HUD.nfsh tables unreadable -- no HUD");
		nfshClose(n);
		return false;
	}
	return true;
}

// Reads one HUD.nfsh record's pixels into `tex`. The file is PICA-swizzled
// 16bpp level 0 already, so this is a read and a copy -- no decoder. Hands
// back the texels (the caller frees them) and their format, NULL when the
// pack has no such record or it does not load.
static u8* nfshTexture(const Nfsh* n, u32 hash, C3D_Tex* tex, Tex3DS_SubTexture* sub, u16* fmtOut) {
	for (u32 i = 0; i < n->count; i++) {
		const u8* r = n->table + i * NFSH_REC;
		if (rd32(r) != hash)
			continue;
		const u16 w = rd16(r + 4), ht = rd16(r + 6), fmt = rd16(r + 8);
		const u32 off = rd32(r + 12);
		// The PICA samples power-of-two textures of at least 8 px only. The
		// packer only halves power-of-two retail art, so this is a guard
		// against a damaged file.
		if (w < 8 || ht < 8 || (w & (w - 1)) || (ht & (ht - 1)) || fmt > NFSH_FMT_RGBA4)
			return NULL;
		const u32 bytes = (u32)w * ht * 2;
		if ((u64)off + bytes > n->blobSize)
			return NULL;
		u8* px = (u8*)malloc(bytes);
		if (!px)
			return NULL;
		const bool ok = fseek(n->f, (long)(n->blobOff + off), SEEK_SET) == 0 &&
			fread(px, 1, bytes, n->f) == bytes &&
			C3D_TexInit(tex, w, ht, fmt == NFSH_FMT_RGB565 ? GPU_RGB565 : GPU_RGBA4);
		if (!ok) {
			free(px);
			return NULL;
		}
		C3D_TexLoadImage(tex, px, GPU_TEXFACE_2D, 0);
		C3D_TexFlush(tex);
		C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		// Same row order and orientation as texpack.c's upload_rgba_tiled(),
		// which car_pack.encode_texture() matches.
		*sub = (Tex3DS_SubTexture){ w, ht, 0.0f, 1.0f, 1.0f, 0.0f };
		*fmtOut = fmt;
		return px;
	}
	return NULL;
}

// One record into the cluster's slot `e`.
static bool loadTexture(Hud* h, const Nfsh* n, u32 hash, int e) {
	u16 fmt = 0;
	u8* px = nfshTexture(n, hash, &h->tex[e], &h->sub[e], &fmt);
	if (!px)
		return false;
	h->texValid[e] = true;
	if (e == HE_NOS_GB || e == HE_NOS_MASK)
		keepNosTexels(h, e, px, h->sub[e].width, h->sub[e].height, fmt);
	free(px);
	return true;
}

// Pulls `n` widgets out of a HUD package's .fng by object hash, `want[e]`
// into `out[e]`.
static int readWidgetsInto(HudWidget* out, int n, const u32* want, const u8* fng, u32 fngLen,
	const LangTable* lang)
{
	FngScreen* s = (FngScreen*)malloc(sizeof(FngScreen));
	if (!s)
		return -1;
	if (!fng_parse(fng, fngLen, s)) {
		free(s);
		return -1;
	}
	int found = 0;
	for (int e = 0; e < n; e++) {
		for (int i = 0; i < s->widgetCount; i++) {
			const FngWidget* fw = &s->widgets[i];
			if (!fw->hasObjHash || fw->objHash != want[e] || !fw->hasPos)
				continue;
			HudWidget* w = &out[e];
			w->found = true;
			w->depth = fw->posDepth;
			// The engine composes the parent chain's transforms; every group
			// above the cluster is unrotated at scale 1, so that is a sum.
			float x = 0.0f, y = 0.0f;
			int p = i;
			for (int guard = 0; p >= 0 && guard < 16; guard++) {
				x += s->widgets[p].posX;
				y += s->widgets[p].posY;
				p = s->widgets[p].parentIndex;
			}
			w->x = x;
			w->y = y;
			w->w = fw->hasSize ? fw->sizeW : 1.0f;
			w->h = fw->hasSize ? fw->sizeH : 1.0f;
			w->rgba[0] = fw->hasColor ? fw->colorR : 255;
			w->rgba[1] = fw->hasColor ? fw->colorG : 255;
			w->rgba[2] = fw->hasColor ? fw->colorB : 255;
			w->rgba[3] = fw->hasColor ? fw->colorA : 255;
			w->color = C2D_Color32(w->rgba[0], w->rgba[1], w->rgba[2], w->rgba[3]);
			w->fontPt = fw->hasFontRef ? fw->fontPointSize : 0;
			w->justify = fw->hasJustify ? fw->justify : 0;
			w->resource = fw->hasResourceRef ? fw->resourceHash : 0;
			const char* t = (lang && fw->hasStringHash) ? langbin_lookup(lang, fw->stringHash) : NULL;
			if (!t && fw->hasLiteralText)
				t = fw->literalText;
			snprintf(w->text, sizeof(w->text), "%s", t ? t : "");
			found++;
			break;
		}
	}
	free(s);
	return found;
}

// The cluster's, out of HUD_SingleRace.fng.
static int readWidgets(Hud* h, const u8* fng, u32 fngLen, const LangTable* lang) {
	u32 want[HE_COUNT];
	for (int e = 0; e < HE_COUNT; e++)
		want[e] = kWidgetName[e] ? presetcar_hash(kWidgetName[e]) : HUD_COUNTDOWN_HASH;
	return readWidgetsInto(h->w, HE_COUNT, want, fng, fngLen, lang);
}

// The race stats', out of the same screen (HudStatElem's order).
static int readStatWidgets(Hud* h, const u8* fng, u32 fngLen, const LangTable* lang) {
	static const char* const kRowName[4] = { "LBData_%d", "LBTextBacking_%d", "LeaderText_%d",
		"LBBacking_%d" };
	u32 want[SE_COUNT];
	want[SE_BACKING] = presetcar_hash("PositionBacking");
	want[SE_NUMBER] = presetcar_hash("RacePositionNumber");
	want[SE_NTH] = presetcar_hash("RacePositionNth");
	want[SE_RACERS] = presetcar_hash("NumberRacers");
	char name[32];
	for (int k = 0; k < 4; k++) {
		for (int i = 0; i < HUD_LB_ROWS; i++) {
			snprintf(name, sizeof(name), kRowName[k], i + 1);
			want[SE_LB_DATA + k * HUD_LB_ROWS + i] = presetcar_hash(name);
		}
	}
	return readWidgetsInto(h->st, SE_COUNT, want, fng, fngLen, lang);
}

// The cluster's draw order, furthest first as the engine draws: every widget
// found, less those `skip` marks (NULL for none).
static void hudSortOrder(Hud* h, const bool* skip) {
	h->orderCount = 0;
	for (int e = 0; e < HE_COUNT; e++) {
		if (!h->w[e].found || (skip && skip[e]))
			continue;
		int j = h->orderCount++;
		while (j > 0 && h->w[h->order[j - 1]].depth < h->w[e].depth) {
			h->order[j] = h->order[j - 1];
			j--;
		}
		h->order[j] = e;
	}
}

// One English.bin string into `out`, or `fallback` when there is no table or
// no such key.
static void langCopy(const LangTable* lang, u32 hash, const char* fallback, char* out, size_t cap) {
	const char* s = lang ? langbin_lookup(lang, hash) : NULL;
	snprintf(out, cap, "%s", s ? s : fallback);
}

// The same, with the table's own '^' line breaks turned into newlines.
static void langCopyLines(const LangTable* lang, u32 hash, const char* fallback, char* out,
	size_t cap)
{
	langCopy(lang, hash, fallback, out, cap);
	for (char* c = out; *c; c++)
		if (*c == '^')
			*c = '\n';
}

Hud* hud_open(const char* nfshPath, const u8* fng, u32 fngLen,
	const HudParts* parts, const HudEngine* eng, bool kph, const char* unitText,
	const LangTable* lang, void (*log)(const char*))
{
	char b[256];
	Hud* h = (Hud*)calloc(1, sizeof(Hud));
	if (!h)
		return NULL;
	// The fallbacks are what English.bin holds for each key, so a card
	// without the language file still reads correctly.
	langCopy(lang, 0x00000535u, "GO!", h->sGo, sizeof(h->sGo));
	langCopy(lang, 0xB1EE4CA9u, "Completed", h->sCompleted, sizeof(h->sCompleted));
	langCopy(lang, 0x61C9B7A2u, "--:--.--", h->sNoTime, sizeof(h->sNoTime));
	langCopy(lang, 0xFC1B498Au, "LAP TIME", h->sLapTime, sizeof(h->sLapTime));
	langCopy(lang, 0xA8247197u, "BEST LAP TIME", h->sBestLapTime, sizeof(h->sBestLapTime));
	langCopy(lang, 0xC2878EBCu, "Your time:", h->sYourTime, sizeof(h->sYourTime));
	langCopy(lang, 0x13640DE9u, "Your score:", h->sYourScore, sizeof(h->sYourScore));
	langCopy(lang, 0x7449D26Du, "Blown engine!", h->sBlown, sizeof(h->sBlown));
	langCopy(lang, 0x4BA0D22Fu, "Totaled!", h->sTotaled, sizeof(h->sTotaled));
	langCopy(lang, 0xABA1E56Fu, "Bank Reward for Magazine", h->sMagReward, sizeof(h->sMagReward));
	langCopy(lang, 0x2AB10D61u, "Bank Reward for DVD", h->sDvdReward, sizeof(h->sDvdReward));
	langCopy(lang, 0x6BB2D6AFu, "Meters", h->sMeters, sizeof(h->sMeters));
	langCopy(lang, 0x001578C3u, "Feet", h->sFeet, sizeof(h->sFeet));
	langCopyLines(lang, 0xBFD1936Fu, "You have the Lead!", h->sLeadGot, sizeof(h->sLeadGot));
	langCopyLines(lang, 0x9B2AF3C7u, "You Lost the Lead!", h->sLeadLost, sizeof(h->sLeadLost));
	langCopyLines(lang, 0x783202B0u, "Outrun Challenge Accepted^Lose Them!", h->sAcceptLose,
		sizeof(h->sAcceptLose));
	langCopyLines(lang, 0x18B97380u, "Outrun Challenge Accepted^Catch Them!", h->sAcceptCatch,
		sizeof(h->sAcceptCatch));
	langCopy(lang, 0xDC089C64u, "You Won the Outrun!", h->sWonOutrun, sizeof(h->sWonOutrun));
	langCopy(lang, 0x5D1621B2u, "You Lost the Outrun!", h->sLostOutrun, sizeof(h->sLostOutrun));
	langCopy(lang, 0x5D10F6AFu, "Bank Reward:", h->sBankReward, sizeof(h->sBankReward));
	static const u32 kOrdinal[4] = { 0x7447C0E3u, 0xBC4CE12Fu, 0x63EB6A03u, 0xBD498562u };
	static const char* const kOrdinalEn[4] = { "th", "st", "nd", "rd" };
	for (int i = 0; i < 4; i++)
		langCopy(lang, kOrdinal[i], kOrdinalEn[i], h->sOrd[i], sizeof(h->sOrd[i]));
	langCopy(lang, 0xBCDD9448u, "th", h->sOrd10, sizeof(h->sOrd10));
	langCopy(lang, 0xD4293449u, "Finished", h->sFinished, sizeof(h->sFinished));
	langCopy(lang, 0x5D82DBA2u, "Knocked Out", h->sKnockedOut, sizeof(h->sKnockedOut));
	langCopy(lang, 0x01E66364u, "Blown Engine", h->sBlownEngine, sizeof(h->sBlownEngine));
	langCopy(lang, 0xB7B75185u, "Totaled", h->sTotaledLb, sizeof(h->sTotaledLb));
	langCopy(lang, 0x1CE129E0u, "Timeup", h->sTimeup, sizeof(h->sTimeup));
	langCopy(lang, 0xA7DB717Fu, "Disconnected", h->sDisconnected, sizeof(h->sDisconnected));
	const int nWidgets = readWidgets(h, fng, fngLen, lang);
	if (nWidgets <= 0) {
		log("hud: HUD_SingleRace.fng did not parse -- no HUD");
		free(h);
		return NULL;
	}
	const int nStats = readStatWidgets(h, fng, fngLen, lang);

	Nfsh n;
	if (!nfshOpen(&n, nfshPath, log)) {
		free(h);
		return NULL;
	}

	// The car's HUD style: the slot-0xA4 part's HUDINDEX, 0 for an empty slot
	// (the constructor's default).
	u32 style = 0;
	bool styleKnown = parts->stylePart == 0;
	for (u32 i = 0; i < n.nStyles && !styleKnown; i++) {
		const u8* r = n.table + n.count * NFSH_REC + i * NFSH_STYLE_REC;
		if (rd32(r) == parts->stylePart) {
			style = rd32(r + 4);
			styleKnown = true;
		}
	}

	// ChooseLoadableTextures (0x80225918): the custom set by style, the plate
	// and the redline band by GetTachStyle.
	const int ts = tachStyle(eng->redlineRpm, eng->limiterRpm);
	static const int kPlateRpm[4] = { 10500, 9500, 8500, 7500 };
	const int plateRpm = kPlateRpm[ts >> 1];
	char name[48];
	u32 want[HE_IMAGES];
	snprintf(name, sizeof(name), "TACH_FILL_CUSTOM_%02u", (unsigned)style);
	want[HE_FILL] = presetcar_hash(name);
	snprintf(name, sizeof(name), "3RDPERSON_REDLINE_%d", 9000 - 500 * ts);
	want[HE_REDLINE] = presetcar_hash(name);
	snprintf(name, sizeof(name), "3RDPERSON_%dLINES_CUSTOM_%02u", plateRpm, (unsigned)style);
	want[HE_PLATE] = presetcar_hash(name);
	snprintf(name, sizeof(name), "3RDPERSON_RPMDIAL_CUSTOM_%02u", (unsigned)style);
	want[HE_NEEDLE] = presetcar_hash(name);
	// Everything else keeps the texture the .fng authors, but the turbo
	// gauge's three, which take the custom set's (local_2C / 24 / 28).
	for (int e = HE_NOS_FILL; e < HE_IMAGES; e++)
		want[e] = h->w[e].resource;
	snprintf(name, sizeof(name), "3RDPERSON_TURBOFILL_CUSTOM_%02u", (unsigned)style);
	want[HE_TURBO_FILL] = presetcar_hash(name);
	snprintf(name, sizeof(name), "3RDPERSON_TURBOLINES_CUSTOM_%02u", (unsigned)style);
	want[HE_TURBO_LINES] = presetcar_hash(name);
	snprintf(name, sizeof(name), "3RDPERSON_TURBODIAL_CUSTOM_%02u", (unsigned)style);
	want[HE_TURBO_DIAL] = presetcar_hash(name);

	int loaded = 0;
	for (int e = 0; e < HE_IMAGES; e++) {
		if (h->w[e].found && want[e] && loadTexture(h, &n, want[e], e))
			loaded++;
	}
	// RacePosition's PositionBacking. LeaderBoard's backings are WHITE16X16
	// tinted, drawn as rectangles.
	if (h->st[SE_BACKING].found && h->st[SE_BACKING].resource) {
		u16 fmt = 0;
		u8* px = nfshTexture(&n, h->st[SE_BACKING].resource, &h->stTex, &h->stSub, &fmt);
		h->stTexValid = px != NULL;
		free(px);
	}
	// The rear-view mirror's frame and cut-out (#501): bStringHash "RVM" and
	// "RVM_MASK", which FUN_005b98b0 loads from HudTexturesGlobal.tpk.
	static const u32 kRvmHash[2] = { 0x0000DBD4u, 0xCF29D0DFu };
	for (int i = 0; i < 2; i++) {
		u16 fmt = 0;
		Tex3DS_SubTexture sub;
		u8* px = nfshTexture(&n, kRvmHash[i], &h->rvmTex[i], &sub, &fmt);
		h->rvmValid[i] = px != NULL;
		free(px);
	}
	if (!h->rvmValid[0] || !h->rvmValid[1])
		log("hud: RVM / RVM_MASK missing from HUD.nfsh (rerun tools/hud_pack.py) -- the "
			"rear-view mirror is drawn without its frame");
	nfshClose(&n);

	// Tints replace the authored colour (FEngSetColor), slot by slot.
	h->w[HE_FILL].color = tintToC2d(parts->tint[0]);
	h->w[HE_NEEDLE].color = tintToC2d(parts->tint[1]);
	h->w[HE_PLATE].color = tintToC2d(parts->tint[2]);
	// And the turbo gauge's: fill as the tach's fill (+0x90), dial as its
	// needle (+0xA0), lines as its plate (+0xB0).
	h->w[HE_TURBO_FILL].color = h->w[HE_FILL].color;
	h->w[HE_TURBO_DIAL].color = h->w[HE_NEEDLE].color;
	h->w[HE_TURBO_LINES].color = h->w[HE_PLATE].color;

	// The gamebreaker composite's two textures, the size of TACH_NOS as packed.
	if (h->nosShape && h->nosMaskA && h->w[HE_NOS_MASK].found) {
		const bool t0 = C3D_TexInit(&h->nosTex[0], h->nosShapeW, h->nosShapeH, GPU_A8);
		const bool t1 = t0 && C3D_TexInit(&h->nosTex[1], h->nosShapeW, h->nosShapeH, GPU_A8);
		if (t0 && !t1)
			C3D_TexDelete(&h->nosTex[0]);
		h->nosTexValid = t1;
		for (int i = 0; i < 2 && h->nosTexValid; i++) {
			memset(h->nosTex[i].data, 0, h->nosTex[i].size);
			C3D_TexFlush(&h->nosTex[i]);
			C3D_TexSetFilter(&h->nosTex[i], GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(&h->nosTex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
		h->nosAngle = NAN;
	}
	if (!h->w[HE_NOS_MASK].found)
		log("hud: this package has no nitrous gauge (HUD_Short_Track, #512)");
	else if (!h->nosTexValid)
		log("hud: no NOS_Mask composite (TACH_NOS / NOS_MASK missing from HUD.nfsh -- "
			"rerun tools/hud_pack.py); the nitrous gauge stays hidden");

	h->limiter = eng->limiterRpm > 0.0f ? eng->limiterRpm : 1.0f;
	h->sweep = h->limiter / (float)plateRpm * HUD_PLATE_SWEEP_DEG;
	h->log = log;
	hud_set_units(h, kph, unitText);
	h->buf = C2D_TextBufNew(256);
	h->stBuf = C2D_TextBufNew(192);
	hudSortOrder(h, NULL);

	snprintf(b, sizeof(b),
		"hud: %d/%d widgets, %d/%d textures, style %lu%s, tach style %d (plate %d, "
		"redline %d), sweep %.1f deg, tints %08lx %08lx %08lx",
		nWidgets, HE_COUNT, loaded, HE_IMAGES, (unsigned long)style,
		styleKnown ? "" : " (part not in the style table)", ts, plateRpm,
		9000 - 500 * ts, h->sweep, (unsigned long)parts->tint[0],
		(unsigned long)parts->tint[1], (unsigned long)parts->tint[2]);
	log(b);
	snprintf(b, sizeof(b), "hud: race stats -- %d/%d widgets, PositionBacking %s", nStats,
		SE_COUNT, h->stTexValid ? "loaded" : "missing (rerun tools/hud_pack.py)");
	log(b);
	snprintf(b, sizeof(b), "hud: GetAwayMeter -- backing %s (%.0f,%.0f), bar %s (%.0f,%.0f), "
		"number (%.0f,%.0f), text (%.0f,%.0f), Out_Run_Message %s (%.0f,%.0f)",
		h->texValid[HE_GA_BACKING] ? "loaded" : "missing", h->w[HE_GA_BACKING].x,
		h->w[HE_GA_BACKING].y, h->texValid[HE_GA_BAR] ? "loaded" : "missing", h->w[HE_GA_BAR].x,
		h->w[HE_GA_BAR].y, h->w[HE_GA_NUMBER].x, h->w[HE_GA_NUMBER].y, h->w[HE_GA_TEXT].x,
		h->w[HE_GA_TEXT].y, h->w[HE_OUTRUN_MSG].found ? "found" : "missing",
		h->w[HE_OUTRUN_MSG].x, h->w[HE_OUTRUN_MSG].y);
	log(b);
	return h;
}

bool hud_rvm_art(const Hud* h, const C3D_Tex** frame, const C3D_Tex** mask) {
	if (!h || !h->rvmValid[0] || !h->rvmValid[1])
		return false;
	*frame = &h->rvmTex[0];
	*mask = &h->rvmTex[1];
	return true;
}

void hud_set_tints(Hud* h, const u32 tint[3]) {
	if (!h)
		return;
	h->w[HE_FILL].color = tintToC2d(tint[0]);
	h->w[HE_NEEDLE].color = tintToC2d(tint[1]);
	h->w[HE_PLATE].color = tintToC2d(tint[2]);
	h->w[HE_TURBO_FILL].color = h->w[HE_FILL].color;
	h->w[HE_TURBO_DIAL].color = h->w[HE_NEEDLE].color;
	h->w[HE_TURBO_LINES].color = h->w[HE_PLATE].color;
}

void hud_set_units(Hud* h, bool kph, const char* unitText) {
	if (!h)
		return;
	h->kph = kph;
	snprintf(h->unitText, sizeof(h->unitText), "%s", unitText ? unitText : (kph ? "KPH" : "MPH"));
}

void hud_close(Hud* h) {
	if (!h)
		return;
	hud_drag_close(h);
	hud_drift_close(h);
	for (int e = 0; e < HE_IMAGES; e++)
		if (h->texValid[e])
			C3D_TexDelete(&h->tex[e]);
	if (h->nosTexValid) {
		C3D_TexDelete(&h->nosTex[0]);
		C3D_TexDelete(&h->nosTex[1]);
	}
	if (h->stTexValid)
		C3D_TexDelete(&h->stTex);
	for (int i = 0; i < 2; i++)
		if (h->rvmValid[i])
			C3D_TexDelete(&h->rvmTex[i]);
	free(h->nosShape);
	free(h->nosMaskA);
	if (h->buf)
		C2D_TextBufDelete(h->buf);
	if (h->stBuf)
		C2D_TextBufDelete(h->stBuf);
	free(h);
}

// Canvas x = 0 on the screen for widget `e`: the cluster's corner mapping,
// the centre line's, or the top-left one's.
static float originX(int e) {
	return isCentred(e) ? HUD_CENTRE_X : isGetawayMeter(e) ? HUD_LEFT_X : HUD_ORIGIN_X;
}
static float screenX(int e, float cx) {
	return originX(e) + cx * HUD_SCALE;
}
static float screenY(float cy) { return HUD_ORIGIN_Y + cy * HUD_SCALE; }

bool hud_gauge_rect(const Hud* h, float* x0, float* y0, float* x1, float* y1) {
	// The dial is the tick plate (HUD_SingleRace: 128 x 128 canvas units at
	// 216,150). The fill's quad is 256 x 256 and mostly empty, offset down and
	// right; the NOS mask, the shift light and the needle run past the dial
	// too. So the plate's box alone, the fill's only without a plate, then the
	// speed / units / gear anchors beside it (inside the plate on the stock
	// layout).
	const int e = h && h->w[HE_PLATE].found && h->texValid[HE_PLATE] ? HE_PLATE : HE_FILL;
	if (!h || !h->w[e].found || !h->texValid[e])
		return false;
	const HudWidget* d = &h->w[e];
	*x0 = screenX(e, d->x - d->w * 0.5f);
	*x1 = screenX(e, d->x + d->w * 0.5f);
	*y0 = screenY(d->y - d->h * 0.5f);
	*y1 = screenY(d->y + d->h * 0.5f);
	static const int text[] = { HE_SPEED, HE_UNITS, HE_GEAR };
	for (int i = 0; i < 3; i++) {
		const HudWidget* w = &h->w[text[i]];
		if (!w->found)
			continue;
		const float x = screenX(text[i], w->x) + 6.0f, y = screenY(w->y);
		if (x > *x1) *x1 = x;
		if (y > *y1) *y1 = y;
	}
	// And the turbo gauge's lines beside it: the Custom Gauges preview always
	// draws the turbo (DisplayCurrentHUD 0x8017B978 sets its three images
	// whatever the car has, #477).
	const HudWidget* t = &h->w[HE_TURBO_LINES];
	if (t->found && h->texValid[HE_TURBO_LINES]) {
		const float tx0 = screenX(HE_TURBO_LINES, t->x - t->w * 0.5f);
		const float tx1 = screenX(HE_TURBO_LINES, t->x + t->w * 0.5f);
		const float ty0 = screenY(t->y - t->h * 0.5f), ty1 = screenY(t->y + t->h * 0.5f);
		if (tx0 < *x0) *x0 = tx0;
		if (tx1 > *x1) *x1 = tx1;
		if (ty0 < *y0) *y0 = ty0;
		if (ty1 > *y1) *y1 = ty1;
	}
	return true;
}

// One Image widget, centred on its authored position.
static void drawImage(Hud* h, int e, float angleDeg) {
	if (!h->texValid[e])
		return;
	const HudWidget* w = &h->w[e];
	const Tex3DS_SubTexture* sub = &h->sub[e];
	C2D_Image img = { &h->tex[e], sub };
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, w->color, 1.0f);
	C2D_DrawImageAtRotated(img, screenX(e, w->x), screenY(w->y), 0.5f,
		angleDeg * (float)M_PI / 180.0f, &tint,
		w->w * HUD_SCALE / (float)sub->width,
		w->h * HUD_SCALE / (float)sub->height);
}

// One String widget: authored point size times its SA scale, justified the way
// FEngFont::RenderString does (see fng.h's Sj note). `scale` multiplies the
// authored size about the justified anchor, which is FEngSetScaleX/Y with the
// centre put back (the Countdown's growth); `dx`/`dy` move it in canvas units.
// The same for any package's widget, canvas (0, 0) at screen (`originX`,
// `originY`) and `px` screen pixels to the canvas unit, into `buf`.
static void drawTextIn(C2D_TextBuf buf, const HudWidget* w, float originX, float originY,
	float px, const char* str, u32 color, C2D_Font font, float fontUnitPx, float scale,
	float dx, float dy)
{
	if (!str[0])
		return;
	C2D_Text t;
	C2D_TextFontParse(&t, font, buf, str);
	C2D_TextOptimize(&t);
	float sc = (w->fontPt ? (float)w->fontPt : 21.0f) * w->h * px / fontUnitPx;
	if (sc < HUD_MIN_TEXT_PX / fontUnitPx)
		sc = HUD_MIN_TEXT_PX / fontUnitPx;
	sc *= scale;
	float tw = 0.0f, th = 0.0f;
	textDims(&t, sc, sc, &tw, &th);
	float x = originX + (w->x + dx) * px, y = originY + (w->y + dy) * px;
	if (w->justify & 0x1)
		x -= tw * 0.5f;
	else if (w->justify & 0x2)
		x -= tw;
	if (w->justify & 0x4)
		y -= th * 0.5f;
	else if (w->justify & 0x8)
		y -= th;
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, sc, sc, color);
}

// The top screen's: canvas x = 0 at screen x `originX`, at the cluster's scale.
static void drawText(Hud* h, const HudWidget* w, float originX, const char* str, u32 color,
	C2D_Font font, float fontUnitPx, float scale, float dx, float dy)
{
	drawTextIn(h->buf, w, originX, HUD_ORIGIN_Y, HUD_SCALE, str, color, font, fontUnitPx,
		scale, dx, dy);
}

static void drawStringAt(Hud* h, int e, const char* str, u32 color, C2D_Font font,
	float fontUnitPx, float scale, float dx, float dy)
{
	drawText(h, &h->w[e], originX(e), str, color, font, fontUnitPx, scale, dx, dy);
}

static void drawString(Hud* h, int e, const char* str, u32 color, C2D_Font font,
	float fontUnitPx, float scale, float dy)
{
	drawStringAt(h, e, str, color, font, fontUnitPx, scale, 0.0f, dy);
}

// CunningStuntSpread's list and strings back to the constructor's.
static void stuntReset(Hud* h) {
	h->spCount = 0;
	memset(&h->spName, 0, sizeof(h->spName));
	memset(&h->spPoints, 0, sizeof(h->spPoints));
}

void hud_race_reset(Hud* h) {
	if (!h)
		return;
	// __10LapCounterPCci zeroes +0x54..+0x68; __15RaceOverMessagePCci +0x28.
	h->lcNewLapAt = h->lcShown = h->lcTitleAt = 0;
	h->lcBest = h->lcHadLap = false;
	// __9SplitTimePCci zeroes +0x28..+0x34.
	h->stZoomed = false;
	h->stZoomAt = h->stSplit = 0;
	h->roStarted = false;
	h->roStartedAt = 0;
	// __10WrongWIndiPCci zeroes +0x28; the image is back on its SA state.
	h->wwShown = false;
	h->wwScript = WW_IDLE;
	// The race's HUD is a new FEngHud, with a new CunningStuntSpread.
	stuntReset(h);
	// __12RacePositionPCci zeroes +0x4C / +0x50. __11LeaderBoardPCci starts
	// on the gaps with +0x1BC the WorldTimer as it is built, which the first
	// hud_draw_race_stats() takes (0 is unset).
	h->rpNewLapAt = h->rpTime = 0;
	h->lbSince = 0;
	h->lbNames = false;
}

static bool tickSet(s32 t) { return t != 0 && t != 0x7FFFFFFF; }

// One track of an FEScript, linear between its keys (FEScript keys are
// milliseconds). `keyMs`/`val` are the keys in order; clamps past either end.
static float scriptTrack(const float* keyMs, const float* val, int n, float ms) {
	if (ms <= keyMs[0])
		return val[0];
	for (int i = 1; i < n; i++) {
		if (ms <= keyMs[i]) {
			const float f = (ms - keyMs[i - 1]) / (keyMs[i] - keyMs[i - 1]);
			return val[i - 1] + (val[i] - val[i - 1]) * f;
		}
	}
	return val[n - 1];
}

// RaceOverMessage's and RaceOverBacking's script 0x657A31F6 -- the one
// Update plays with FEngSetScript the first time it runs (0x8010E1B4) --
// decoded out of HUD_SingleRace.fng: four keys at 0/200/400/600 ms on the
// colour, position and size tracks. Base + delta per key:
//   message colour (220,220,220, 505 + {-505,-250,-305,-250}) -> alpha
//           0, 255, 200, 255; position (-1,-40); size 1 + {-0.3,0,-0.3,0}
//   backing colour (0,0,128,5) + {(0,0,-128,-5), (..,195), (..,45),
//           (..,145)} -> black at alpha 0, 200, 50, 150; position (0,0),
//           size 1070 x 650
static const float kRoKeyMs[4] = { 0.0f, 200.0f, 400.0f, 600.0f };
static const float kRoMsgAlpha[4] = { 0.0f, 255.0f, 200.0f, 255.0f };
static const float kRoMsgScale[4] = { 0.7f, 1.0f, 0.7f, 1.0f };
static const float kRoBackAlpha[4] = { 0.0f, 200.0f, 50.0f, 150.0f };
#define HUD_RO_MSG_GREY 220
// Cash_Reward's ZoomIn (0x8AB83EDB), the script Update starts on it,
// decoded out of HUD_SingleRace.fng: 1800 ms. Colour (138,244,202) at alpha
// 360 + {-360 at 0, -105 at 300, -105 at 1720, -360 at 1800} -> 0, 255, 255,
// 0; position (7,-53); scale 0.95 + {-0.8 at 0, -0.3 at 300} -> 0.15 growing
// to 0.65, which it keeps. Past the last key the string is up at alpha 0.
static const float kCrKeyMs[4] = { 0.0f, 300.0f, 1720.0f, 1800.0f };
static const float kCrAlpha[4] = { 0.0f, 255.0f, 255.0f, 0.0f };
static const float kCrScaleKeyMs[2] = { 0.0f, 300.0f };
static const float kCrScale[2] = { 0.15f, 0.65f };
#define HUD_CR_MS 1800.0f
#define HUD_CR_Y  (-53.0f)

// GetAwayMeter (#268 H step 2), out of HUD_SingleRace.fng and Update.
// The bar's colours, 0xAARRGGBB (FEngSetColor 0xFF30CF30 ahead, 0xFF950000
// behind) and SetHorizontalBarValue's (v, 1, 99, 1).
#define HUD_GA_AHEAD   0xFF30CF30u
#define HUD_GA_BEHIND  0xFF950000u
#define HUD_GA_BAR_LO  99.0f
#define HUD_GA_BAR_HI  1.0f
// ConvertMetersToFeet.
#define HUD_M_TO_FT    3.2808399f
// Distance_Number's "Ahead" (0x026BB992) and "Behind" (0x520393A9), the same
// looping 800 ms: colour (192,192,192) alpha 255 + {0, -55, 0} and size
// 0.7 + {0, -0.2, 0} at 0 / 400 / 800 ms.
#define HUD_GA_PULSE_MS 800.0f
static const float kGaPulseKeyMs[3] = { 0.0f, 400.0f, 800.0f };
static const float kGaPulseAlpha[3] = { 255.0f, 200.0f, 255.0f };
static const float kGaPulseSize[3] = { 0.7f, 0.5f, 0.7f };
// Out_Run_Message's ZoomIn (0x8AB83EDB), 1800 ms: the alpha keys
// Cash_Reward's has (kCrKeyMs / kCrAlpha: 0, 255 by 300 ms, held to 1720, 0
// at 1800), at (0, -86) where the SA state has (0, -89), size 1.12 +
// {-0.8 at 0, -0.3 at 300} -> 0.32 growing to 0.82.
#define HUD_OR_MS 1800.0f
#define HUD_OR_Y  (-86.0f)
static const float kOrSize[2] = { 0.32f, 0.82f };
// SplitTime's ZoomIn scripts (#502), decoded out of HUD_SingleRace.fng
// (SplitTimeText 0xF59D909F, a string at (0, -86)), both 1800 ms, linear,
// chaining to INIT (alpha 0): the alpha 0 -> 255 by 600 ms and held, the
// size 1.1 -> 1.2 by 600 ms. ZoominGreen is (0, 255, 0), ZoominRed
// (170, 128, 0) -- FEColor's B, G, R base 255 72 24 + the keys' deltas.
// Update hides the string 3 s (0x803264D4) after ZoomIn started it.
#define HUD_ST_MS       1800.0f
#define HUD_ST_HIDE_SEC 3.0f
#define HUD_ST_GREEN    0x00FF00u
#define HUD_ST_RED      0xAA8000u
static const float kStKeyMs[2] = { 0.0f, 600.0f };
static const float kStAlpha[2] = { 0.0f, 255.0f };
static const float kStSize[2] = { 1.1f, 1.2f };
#define HUD_RO_MSG_Y   (-40.0f)
#define HUD_RO_BACK_W  1070.0f
#define HUD_RO_BACK_H  650.0f

// WrongWayImage's scripts, decoded out of HUD_SingleRace.fng (FOb at 46300),
// all linear. The image's own SA state -- white at alpha 0, 54 x 54 at
// (-223, 34) -- is the 0x001744B3 script, so it is invisible until FadeIn.
//   FadeIn  0x5B0D9106, 1200 ms, then the 0x0280164F loop: pops up at the
//           screen's centre (0, -87), 10 -> 64 -> 50 -> 64, alpha 35 -> 255
//           -> 150 -> 255 by 800 ms, then shrinks to 54 on its way to
//           (-223, 34), above where retail's minimap sits.
//   loop    0x0280164F, 800 ms, flags 1 (loops): alpha 150 -> 255 -> 150,
//           size 54 -> 64 -> 54, at (-223, 34).
//   FadeOut 0xBCBFCC87, 200 ms, no chain: alpha 255 -> 0, size 54 -> 44.
#define HUD_WW_IN_MS   1200.0f
#define HUD_WW_LOOP_MS 800.0f
#define HUD_WW_REST_X  (-223.0f)
#define HUD_WW_REST_Y  34.0f
static const float kWwInAlphaMs[4] = { 0.0f, 400.0f, 620.0f, 800.0f };
static const float kWwInAlpha[4] = { 35.0f, 255.0f, 150.0f, 255.0f };
static const float kWwInMs[5] = { 0.0f, 400.0f, 620.0f, 800.0f, 1200.0f };
static const float kWwInX[5] = { 0.0f, 0.0f, 0.0f, 0.0f, HUD_WW_REST_X };
static const float kWwInY[5] = { -87.0f, -87.0f, -87.0f, -87.0f, HUD_WW_REST_Y };
static const float kWwInSize[5] = { 10.0f, 64.0f, 50.0f, 64.0f, 54.0f };
static const float kWwLoopMs[3] = { 0.0f, 400.0f, 800.0f };
static const float kWwLoopAlpha[3] = { 150.0f, 255.0f, 150.0f };
static const float kWwLoopSize[3] = { 54.0f, 64.0f, 54.0f };
static const float kWwOutMs[2] = { 0.0f, 200.0f };
static const float kWwOutAlpha[2] = { 255.0f, 0.0f };
static const float kWwOutSize[2] = { 54.0f, 44.0f };

// WrongWIndi::Update (0x8010BFA8): FadeIn once when the flag goes up, FadeOut
// once when it comes down. The script steps are FEngSetScript's -- FadeIn
// hands over to its chained loop when it runs out.
static void wrongWayUpdate(Hud* h, bool wrongWay, u64 nowMs) {
	if (!wrongWay) {
		if (h->wwShown && h->wwScript != WW_FADE_OUT) {
			h->wwScript = WW_FADE_OUT;
			h->wwStartMs = nowMs;
			h->wwShown = false;
		}
	} else if (!h->wwShown) {
		h->wwShown = true;
		h->wwScript = WW_FADE_IN;
		h->wwStartMs = nowMs;
	}
	if (h->wwScript == WW_FADE_IN && (float)(nowMs - h->wwStartMs) >= HUD_WW_IN_MS) {
		h->wwScript = WW_LOOP;
		h->wwStartMs += (u64)HUD_WW_IN_MS;
	}
}

static void drawWrongWay(Hud* h, u64 nowMs) {
	const int e = HE_WRONG_WAY;
	if (h->wwScript == WW_IDLE || !h->texValid[e])
		return;
	const float ms = (float)(nowMs - h->wwStartMs);
	float x = HUD_WW_REST_X, y = HUD_WW_REST_Y, size, alpha;
	if (h->wwScript == WW_FADE_IN) {
		alpha = scriptTrack(kWwInAlphaMs, kWwInAlpha, 4, ms);
		x = scriptTrack(kWwInMs, kWwInX, 5, ms);
		y = scriptTrack(kWwInMs, kWwInY, 5, ms);
		size = scriptTrack(kWwInMs, kWwInSize, 5, ms);
	} else if (h->wwScript == WW_LOOP) {
		const float t = fmodf(ms, HUD_WW_LOOP_MS);
		alpha = scriptTrack(kWwLoopMs, kWwLoopAlpha, 3, t);
		size = scriptTrack(kWwLoopMs, kWwLoopSize, 3, t);
	} else {
		alpha = scriptTrack(kWwOutMs, kWwOutAlpha, 2, ms);
		size = scriptTrack(kWwOutMs, kWwOutSize, 2, ms);
		if (ms >= kWwOutMs[1]) {
			h->wwScript = WW_IDLE;
			return;
		}
	}
	const Tex3DS_SubTexture* sub = &h->sub[e];
	C2D_Image img = { &h->tex[e], sub };
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, C2D_Color32(255, 255, 255, (u8)alpha), 1.0f);
	const float px = size * HUD_SCALE;
	C2D_DrawImageAt(img, screenX(e, x) - px * 0.5f, screenY(y) - px * 0.5f, 0.5f, &tint,
		px / (float)sub->width, px / (float)sub->height);
}

// NOS_GameBreaker with NOS_Mask over it, as psNOSGaugeAlphaMask (0x801E6880)
// draws the pair: a gamebreaker texel shows where TACH_NOS clears NOSMaskClip,
// at the alpha of the mask texel over it, in the gamebreaker's colour. The
// mask is its own 256 x 256 object round the arc's centre, turned by
// FEngSetRotationZ (positive clockwise on screen); each gamebreaker texel is
// taken back through that turn into the mask's art. Only the half disc's
// straight edge ever crosses the arc -- every TACH_NOS texel lies inside its
// round one.
static void drawNosGameBreaker(Hud* h, float angleDeg, u32 argb) {
	if (!h->nosTexValid || !h->texValid[HE_NOS_GB])
		return;
	const HudWidget* gb = &h->w[HE_NOS_GB];
	const HudWidget* mk = &h->w[HE_NOS_MASK];
	if (!(fabsf(angleDeg - h->nosAngle) < 0.25f)) {
		const int next = h->nosCur ^ 1;
		u8* dst = (u8*)h->nosTex[next].data;
		const u32 w = h->nosShapeW, ht = h->nosShapeH;
		const float a = angleDeg * (float)M_PI / 180.0f, ca = cosf(a), sa = sinf(a);
		const float sx = (float)h->nosMaskW / mk->w, sy = (float)h->nosMaskH / mk->h;
		for (u32 y = 0; y < ht; y++) {
			const float wy = gb->y + (((float)y + 0.5f) / (float)ht - 0.5f) * gb->h - mk->y;
			for (u32 x = 0; x < w; x++) {
				u8 v = 0;
				if (h->nosShape[y * w + x]) {
					const float wx = gb->x + (((float)x + 0.5f) / (float)w - 0.5f) * gb->w - mk->x;
					const int mx = (int)floorf((wx * ca + wy * sa) * sx + 0.5f * h->nosMaskW);
					const int my = (int)floorf((wy * ca - wx * sa) * sy + 0.5f * h->nosMaskH);
					if (mx >= 0 && my >= 0 && mx < h->nosMaskW && my < h->nosMaskH)
						v = h->nosMaskA[my * h->nosMaskW + mx];
				}
				dst[tiledIndex(x, y, w)] = v;
			}
		}
		C3D_TexFlush(&h->nosTex[next]);
		h->nosCur = next;
		h->nosAngle = angleDeg;
	}
	C2D_Image img = { &h->nosTex[h->nosCur], &h->sub[HE_NOS_GB] };
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, argbToC2d(argb), 1.0f);
	const Tex3DS_SubTexture* sub = &h->sub[HE_NOS_GB];
	C2D_DrawImageAtRotated(img, screenX(HE_NOS_GB, gb->x), screenY(gb->y), 0.5f, 0.0f, &tint,
		gb->w * HUD_SCALE / (float)sub->width, gb->h * HUD_SCALE / (float)sub->height);
}

// ---- CunningStuntSpread (0x8010F1E0..0x801105CC) ----------------------------

// SP_Trick_Type's and SP_Trick_Point's scripts, decoded out of
// HUD_SingleRace.fng (tools/re/wip311/scrdump.py), all linear and the same on
// both strings. The tracks are deltas on the string's base: its colour (the
// one FEngSetColor gave it, alpha 255), its position and its 0.9 size.
//   animate      0x5A8E4EBE, 600 ms: alpha -255 -> 0 by 400; x +30 -> -10 ->
//                0 at 0/400/600; size +0.1 -> +0.3 -> 0.
//   FadeOut      0xBCBFCC87 and fadeoutCrash 0xA8523938, 900 ms: held to
//                400; x -10 at 600, back to 0 at 900; y +80 at 900; alpha
//                -255 at 900; size +0.2 at 600, -0.6 at 900.
// A finished script holds its last key, so a faded string stays invisible.
#define HUD_SP_ANIMATE_MS 600.0f
#define HUD_SP_FADEOUT_MS 900.0f
static const float kSpAnimMs[3] = { 0.0f, 400.0f, 600.0f };
static const float kSpAnimAlpha[3] = { -255.0f, 0.0f, 0.0f };
static const float kSpAnimX[3] = { 30.0f, -10.0f, 0.0f };
static const float kSpAnimSize[3] = { 0.1f, 0.3f, 0.0f };
static const float kSpFadeMs[3] = { 400.0f, 600.0f, 900.0f };
static const float kSpFadeAlpha[3] = { 0.0f, 0.0f, -255.0f };
static const float kSpFadeX[3] = { 0.0f, -10.0f, 0.0f };
static const float kSpFadeY[3] = { 0.0f, 0.0f, 80.0f };
static const float kSpFadeSize[3] = { 0.0f, 0.2f, -0.6f };

static float stuntScriptMs(const HudStuntStr* s, s32 now) {
	return (float)(now - s->startedAt) * 0.25f;
}

// FEngIsScriptRunning: that script, and not yet past its length.
static bool stuntScriptRunning(const HudStuntStr* s, int script, s32 now) {
	if (s->script != script)
		return false;
	const float len = script == SP_SCRIPT_ANIMATE ? HUD_SP_ANIMATE_MS : HUD_SP_FADEOUT_MS;
	return stuntScriptMs(s, now) < len;
}

// FEngSetScript(obj, name, true): from its first key.
static void stuntScriptSet(HudStuntStr* s, int script, s32 now) {
	s->script = script;
	s->startedAt = now;
}

// EA's "%$d" (main.c's formatCashGrouped has the decode): the digits grouped
// by ',' in threes, with the sign.
static void stuntGrouped(int v, char* out, size_t cap) {
	char digits[16];
	const unsigned int m = v < 0 ? 0u - (unsigned int)v : (unsigned int)v;
	const int n = snprintf(digits, sizeof(digits), "%u", m);
	size_t o = 0;
	if (v < 0 && o + 1 < cap)
		out[o++] = '-';
	for (int i = 0; i < n && o + 1 < cap; i++) {
		if (i > 0 && ((n - i) % 3) == 0 && o + 2 < cap)
			out[o++] = ',';
		out[o++] = digits[i];
	}
	out[o] = '\0';
}

// UpdateStylePoints (0x8010FFD0), on the moments StylePointsAccumulator::
// Update listed: each one with points updates the first node of its type, or
// becomes a new node -- at the tail with points of 0 or more, at the head
// below 0. In a drift the list goes to UpdateStylePoints' drift arm instead
// (driftUpdate).
static void stuntUpdateStylePoints(Hud* h, const StyleUp* up, int n) {
	for (int i = 0; i < n; i++) {
		const StyleUp* m = &up[i];
		if (m->points == 0.0f)
			continue;
		HudStunt* s = NULL;
		for (int k = 0; k < h->spCount; k++) {
			if (h->sp[k].type == m->type) {
				s = &h->sp[k];
				break;
			}
		}
		if (s) {
			s->aborted = m->state == STYLE_STATE_ABORTED;
			s->state = m->state;
			s->points = m->points;
			if (m->state != STYLE_STATE_DONE)
				s->mult = (int)(m->mult + 0.5f);
			s->shownAt = 0;
			continue;
		}
		if (h->spCount >= HUD_SP_MAX)
			continue;
		if (m->points >= 0.0f) {
			s = &h->sp[h->spCount++];
		} else {
			memmove(&h->sp[1], &h->sp[0], (size_t)h->spCount * sizeof(h->sp[0]));
			h->spCount++;
			s = &h->sp[0];
		}
		memset(s, 0, sizeof(*s));
		s->continuous = m->continuous;
		s->points = m->points;
		s->mult = (int)(m->mult + 0.5f);
		s->type = m->type;
		s->nameHash = m->nameHash;
		// PORT: retail leaves [9] as the allocator found it; the moment's
		// state is what the update arm would have put there.
		s->state = m->state;
		s->aborted = m->state == STYLE_STATE_ABORTED;
		snprintf(s->name, sizeof(s->name), "%s", m->name ? m->name : "");
	}
}

// DrawStunt (0x801103A4): the moment's name and points into the two strings,
// their colours, and the scripts. A node's first draw plays "animate"; from
// its second, a finished moment -- or any one-off -- starts "fadeout" and is
// parked in state 5. `showPoints` is the constructor's +0x7C.
static void stuntDraw(Hud* h, HudStunt* s, s32 now, bool showPoints) {
	const bool whole = s->nameHash == HUD_SP_WHOLE_HASH;
	// PORT: a multiplier of 0 would divide by zero; retail's never is
	// (ClearMultiplier puts 1).
	const float shown = whole || s->mult == 0 ? s->points : s->points / (float)s->mult;
	char v[16], x[16];
	stuntGrouped((int)shown, v, sizeof(v));
	stuntGrouped(s->mult, x, sizeof(x));
	const bool red = s->aborted || s->points < 0.0f;
	const char* sign = red ? "" : "+";
	if (s->mult < 2 || whole)
		snprintf(h->spPoints.text, sizeof(h->spPoints.text), "%s%s", sign, v);
	else
		snprintf(h->spPoints.text, sizeof(h->spPoints.text), "%s%sx%s", sign, v, x);
	h->spPoints.color = h->spName.color = red ? HUD_SP_RED : HUD_SP_GREEN;
	snprintf(h->spName.text, sizeof(h->spName.text), "%s", s->name);

	if (s->drawn != 1 && !stuntScriptRunning(&h->spPoints, SP_SCRIPT_FADEOUT, now)) {
		stuntScriptSet(&h->spPoints, SP_SCRIPT_ANIMATE, now);
		h->spPoints.visible = showPoints;
	}
	if (s->drawn != 1 && !stuntScriptRunning(&h->spName, SP_SCRIPT_FADEOUT, now)) {
		stuntScriptSet(&h->spName, SP_SCRIPT_ANIMATE, now);
		h->spName.visible = true;
	}
	if (s->drawn != 0 && (s->state == STYLE_STATE_DONE ||
			(!s->continuous && s->state != HUD_SP_STATE_FADING))) {
		stuntScriptSet(&h->spName, SP_SCRIPT_FADEOUT, now);
		if (showPoints)
			stuntScriptSet(&h->spPoints, s->points >= 0.0f ? SP_SCRIPT_FADEOUT
			                                                : SP_SCRIPT_FADEOUT_CRASH, now);
		s->state = HUD_SP_STATE_FADING;
	}
	if (s->drawn != 1) {
		char b[160];
		snprintf(b, sizeof(b), "hud: stunt %s %s%s", h->spName.text, h->spPoints.text,
			showPoints ? "" : " (points hidden: no nitrous)");
		h->log(b);
	}
	if (!tickSet(s->shownAt)) {
		s->drawn = 1;
		s->shownAt = now;
	}
}

// UpdateStunts (0x8010FB10), the non-drift arm. The first one-off on the list
// (or the first node) is drawn unless the name is fading out, and comes off
// the list once it has gone unupdated for a second (half that with more
// queued). A node never drawn since its update has [6] == 0, so while a
// fadeout runs every node queued behind it is dropped at once, as in retail.
// The Tach_NOS_Gamebreaker GLOW / GLOW_2 checks are left out: nothing in
// Speed.elf starts either script, so they never hold the strings.
static void stuntUpdate(Hud* h, s32 now, bool showPoints) {
	if (h->spCount == 0) {
		if (!stuntScriptRunning(&h->spName, SP_SCRIPT_ANIMATE, now) &&
				!stuntScriptRunning(&h->spName, SP_SCRIPT_FADEOUT, now)) {
			h->spName.visible = false;
			h->spPoints.visible = false;
		}
		return;
	}
	int pick = 0;
	for (int k = 0; k < h->spCount; k++) {
		if (!h->sp[k].continuous) {
			pick = k;
			break;
		}
	}
	HudStunt* s = &h->sp[pick];
	if (!stuntScriptRunning(&h->spName, SP_SCRIPT_FADEOUT, now))
		stuntDraw(h, s, now, showPoints);
	const float since = (float)(now - s->shownAt) * 0.00025f;
	if (since > (h->spCount > 1 ? HUD_SP_HOLD_MANY : HUD_SP_HOLD_ONE)) {
		h->spCount--;
		memmove(&h->sp[pick], &h->sp[pick + 1], (size_t)(h->spCount - pick) * sizeof(h->sp[0]));
	}
}

// One of the two strings as its script has it now, on widget `w` of either
// package.
static void stuntDrawText(Hud* h, const HudWidget* w, float originX, const HudStuntStr* s,
	s32 now, C2D_Font font, float fontUnitPx)
{
	if (!s->visible || !s->text[0])
		return;
	float da = 0.0f, dx = 0.0f, dy = 0.0f, ds = 0.0f;
	const float ms = stuntScriptMs(s, now);
	if (s->script == SP_SCRIPT_ANIMATE) {
		da = scriptTrack(kSpAnimMs, kSpAnimAlpha, 3, ms);
		dx = scriptTrack(kSpAnimMs, kSpAnimX, 3, ms);
		ds = scriptTrack(kSpAnimMs, kSpAnimSize, 3, ms);
	} else if (s->script != SP_SCRIPT_NONE) {
		da = scriptTrack(kSpFadeMs, kSpFadeAlpha, 3, ms);
		dx = scriptTrack(kSpFadeMs, kSpFadeX, 3, ms);
		dy = scriptTrack(kSpFadeMs, kSpFadeY, 3, ms);
		ds = scriptTrack(kSpFadeMs, kSpFadeSize, 3, ms);
	}
	const float a = (float)(s->color >> 24) + da;
	if (a < 1.0f)
		return;
	const float base = w->h > 0.0f ? w->h : 1.0f;
	const float scale = (base + ds) / base * HUD_NOTICE(HUD_NOTICE_STUNT);
	if (scale <= 0.0f)
		return;
	const u32 c = (s->color & 0x00FFFFFFu) | ((u32)(a > 255.0f ? 255.0f : a) << 24);
	drawText(h, w, originX, s->text, argbToC2d(c), font, fontUnitPx, scale, dx, dy);
}

static void stuntDrawStr(Hud* h, int e, const HudStuntStr* s, s32 now, C2D_Font font,
	float fontUnitPx)
{
	stuntDrawText(h, &h->w[e], isCentred(e) ? HUD_CENTRE_X : HUD_ORIGIN_X, s, now, font,
		fontUnitPx);
}

// ---- the drag race's package, HUD_Drag.fng (TASKS.md #268 D) ----------------
// What FEngHud's constructor builds over InGameDrag.bun's screen in a drag:
// DragTachometer (__14DragTachometerPCci 0x8010C080, Update 0x8010C458) down
// the left edge, EngineTempGauge (0x8010D0A4 / 0x8010D13C) beside it,
// NitrousGauge's one-player drag arm (0x8010D324) on the right, and the
// Countdown, RaceOverMessage and CunningStuntSpread the single race has too.
// RacePosition registers its strings here as well, and in a one-viewport drag
// Drag_Position_Backing_01/02. DeterminRaceInfo (0x80216344) gives it its bit
// (0x200) with the Position row on, unless TheRaceParameters' four car counts
// (+0x24..+0x30) sum to 1 -- a race of one car -- outside drift, so a drag
// against the field shows it. The place and Backing_01 go to the bottom
// screen (hud_draw_race_stats); RaceTime and Backing_02 stay on the top
// screen's right edge (CONTINUE.MD 1a #18).

typedef enum {
	// Images first: their index is also their texture slot.
	DE_FILL, DE_PLATE, DE_REDLINE, DE_NEEDLE, DE_SHIFT, DE_HEAT,
	DE_NOS_FILL, DE_NOS_LINE, DE_NOS_GB, DE_SPEED_BACKING,
	DE_POS_BACKING, DE_TIME_BACKING,
	DE_IMAGES,
	// White16x16 tinted: drawn as a solid rectangle.
	DE_RACE_OVER_BACKING = DE_IMAGES,
	DE_GEAR, DE_SHIFT_MSG, DE_SPEED, DE_UNITS, DE_COUNTDOWN, DE_RACE_OVER,
	DE_SP_TYPE, DE_SP_POINT,
	DE_RACE_TIME, DE_POS_NUMBER, DE_POS_NTH, DE_POS_RACERS,
	DE_COUNT
} HudDragElem;

static const char* const kDragName[DE_COUNT] = {
	"RPM_fill",                 // DragTachometer, registered plain
	"3rdPersonTachRPMNumbers",  // +0x2C
	"RPM_Redline",              // +0x28
	"3rdPersonNeedle",          // +0x20
	"Shift_light",              // +0x30
	"HEAT_Bar",                 // EngineTempGauge +0x20
	"NOS_Fill",                 // NitrousGauge
	"NOS_Line",                 // +0x20
	"NOS_GameBreaker",          // +0x24
	"Drag_Position_Backing_03", // DragTachometer, registered plain
	"Drag_Position_Backing_01", // RacePosition, registered plain
	"Drag_Position_Backing_02", // RacePosition, registered plain
	"RaceOverBacking",          // RaceOverMessage +0x24
	"3rdPersonGear",            // DragTachometer +0x70
	"ShiftMessage",             // +0x74
	"3rdPersonSpeed",           // +0x7C
	"3rdPersonSpeedUnits",      // +0x78
	NULL,                       // Countdown +0x20
	"RaceOverMessage",          // RaceOverMessage +0x20
	"SP_Trick_Type",            // CunningStuntSpread +0x38
	"SP_Trick_Point",           // +0x34
	"RaceTime",                 // RacePosition +0x38
	"RacePositionNumber",       // +0x28
	"RacePositionNth",          // +0x2C
	"NumberRacers",             // +0x30
};

// The Gauges row's: DragTachometer (feature mask 0x10000060),
// EngineTempGauge (0x20000000, which DetermineGauges adds in a drag with
// Gauges on) and NitrousGauge (0x00040000 in the high word).
static bool isDragGaugeElem(int e) {
	return e <= DE_SPEED_BACKING || (e >= DE_GEAR && e <= DE_UNITS);
}

// RacePosition's, which the bottom screen draws instead.
static bool isDragBottomElem(int e) {
	return e == DE_POS_BACKING || e >= DE_POS_NUMBER;
}

// Where canvas x = 0 lands on the top screen, at the cluster's half scale.
// The tach, the heat bar and the gear letter are authored against the
// canvas's left edge, so that edge goes on the screen's -- the cluster's
// corner mapping (hud.h) mirrored. The nitrous bar, the speed box and the
// style strings keep the right edge's; the shift light and the centre-line
// strings go on the screen's middle, as isCentred()'s do.
#define HUD_DRAG_LEFT_X (320.0f * HUD_SCALE)
static float dragOriginX(int e) {
	switch (e) {
	case DE_FILL: case DE_PLATE: case DE_REDLINE: case DE_NEEDLE: case DE_HEAT: case DE_GEAR:
		return HUD_DRAG_LEFT_X;
	case DE_SHIFT: case DE_SHIFT_MSG: case DE_COUNTDOWN: case DE_RACE_OVER:
	case DE_RACE_OVER_BACKING:
		return HUD_CENTRE_X;
	default:
		return HUD_ORIGIN_X;
	}
}

// The needle turns about a point 210 canvas units right of its art's centre
// (its .fng pivot): the 128-unit needle's tip then runs 274 out, along the
// plate's scale.
#define HUD_DRAG_PIVOT_DX 210.0f

// DragTachometer::Update's colours, 0xAARRGGBB: the gear while the clutch is
// out, and the needle past the shift window (potential bonus -1) and inside
// its top half (above 0.5).
#define HUD_DRAG_GEAR_SHIFTING 0xFFFF8000u
#define HUD_DRAG_NEEDLE_OVER   0xFF800000u
#define HUD_DRAG_NEEDLE_GOOD   0xFF00FF00u
// Blend's target (0x803266B4..BC, R 0x803266A4) as A, R, G, B: below the
// window's top half the needle runs from its authored grey toward this blue.
static const float kDragNeedleBlue[4] = { 255.0f, 0.0f, 128.0f, 192.0f };
// The light's window is never narrower than this (0x803266B0).
#define HUD_DRAG_MIN_RANGE 500.0f

// The package's scripts, decoded out of HUD_Drag.fng (tools/re/wip311/
// scrdump.py), all linear; the colour tracks are deltas on the base.
//   ShiftMessage ZoomIn 0x8AB83EDB, 600 ms: grey 220 at alpha 0, 255 by
//           300, held to 540, 0 at 600; size 1.12 + {-0.8, -0.12} -> 0.32
//           growing to 1.0 by 300, which it keeps. INIT leaves it at alpha 0.
//   RPM_Redline Blink 0x0280164F, 600 ms, loops: green and blue 0, alpha
//           150, red 255 / 150 in two 300 ms beats. INIT is its authored
//           (117, 11, 11) at 200.
//   Shift_light PulseGreen 0x2A15F4B9 / PulseRed 0xA6797F23 / PulseBlue
//           0x75A0BDB0, 600 ms each: (0, 255, 0) / (128, 0, 0) / (0, 128,
//           192) at alpha 255 to 320 ms, 0 from 340. INIT (white) chains
//           to PulseGreen.
//   RaceOverMessage 0x657A31F6: the single race's keys, at (0, -26).
#define HUD_DRAG_ZOOM_MS 600.0f
static const float kDragZoomMs[4] = { 0.0f, 300.0f, 540.0f, 600.0f };
static const float kDragZoomAlpha[4] = { 0.0f, 255.0f, 255.0f, 0.0f };
static const float kDragZoomSizeMs[2] = { 0.0f, 300.0f };
static const float kDragZoomSize[2] = { 0.32f, 1.0f };
#define HUD_DRAG_ZOOM_GREY 220
#define HUD_DRAG_BLINK_MS 600.0f
static const float kDragBlinkMs[8] = { 0.0f, 140.0f, 160.0f, 280.0f, 300.0f, 440.0f, 460.0f, 600.0f };
static const float kDragBlinkRed[8] = { 255.0f, 255.0f, 150.0f, 150.0f, 255.0f, 255.0f, 150.0f, 150.0f };
#define HUD_DRAG_BLINK_ALPHA 150
#define HUD_DRAG_PULSE_MS 600.0f
static const float kDragPulseMs[4] = { 0.0f, 320.0f, 340.0f, 600.0f };
static const float kDragPulseAlpha[4] = { 255.0f, 255.0f, 0.0f, 0.0f };
enum { DL_GREEN, DL_RED, DL_BLUE };
static const u32 kDragPulseRgb[3] = { 0x00FF00u, 0x800000u, 0x0080C0u };
#define HUD_DRAG_RO_MSG_Y (-26.0f)

// SetVerticalBarValue's ranges, (lo, hi) in texels of the art's height:
// EngineTempGauge's one-player pair (0x803266F0 / F4) and NitrousGauge's
// drag pair (0x80326738 / 3C).
#define HUD_DRAG_HEAT_LO 105.0f
#define HUD_DRAG_HEAT_HI 5.0f
#define HUD_DRAG_NOS_LO  45.0f
#define HUD_DRAG_NOS_HI  (-9.0f)

struct HudDrag {
	HudWidget w[DE_COUNT];
	int order[DE_COUNT];
	int orderCount;
	C3D_Tex tex[DE_IMAGES];
	Tex3DS_SubTexture sub[DE_IMAGES];
	bool texValid[DE_IMAGES];

	bool plateMissing;       // +0xB8: no DRAG_RPM_%d_LINES for the car's tach style
	float angStart, angEnd;  // CarHudInfo +0xC / +0x10, the one-player sweep
	float redline, limiter;  // EngineInfo +4 / +8
	u32 gearBase;            // +0xAC, the gear letter's authored colour (ARGB)
	u32 needleBase;          // +0xB0, the needle's
	u32 needleColor;         // what FEngSetColor last left on the needle
	int lastGear;            // +0x80, the engine's numbering (0 R, 1 N, 2 first)
	bool msgUp;              // +0x88, a Perfect / Good Shift! is up
	bool grindUp;            // +0x8C, Missed Shift!
	bool shortShown;         // +0x90
	bool overShown;          // +0x94
	bool blinking;           // +0x9C, Blink is on RPM_Redline
	u64 blinkMs;

	// ShiftMessage: shown, ZoomIn set on it (else INIT) and when, its text.
	bool msgVisible;
	bool msgZoom;
	u64 msgMs;
	const char* msgText;
	// Shift_light: shown, its pulse and when it started.
	bool lightVisible;
	int lightScript;
	u64 lightMs;

	char sPerfect[24], sGood[24], sMissed[24], sShort[24], sOver[24];
};

static void dragFree(struct HudDrag* d) {
	if (!d)
		return;
	for (int e = 0; e < DE_IMAGES; e++)
		if (d->texValid[e])
			C3D_TexDelete(&d->tex[e]);
	free(d);
}

void hud_drag_close(Hud* h) {
	if (!h || !h->drag)
		return;
	dragFree(h->drag);
	h->drag = NULL;
}

static u32 widgetArgb(const HudWidget* w) {
	return (u32)w->rgba[3] << 24 | (u32)w->rgba[0] << 16 | (u32)w->rgba[1] << 8 | w->rgba[2];
}

bool hud_drag_open(Hud* h, const char* nfshPath, const u8* fng, u32 fngLen,
	const HudEngine* eng, float angStart, float angEnd, const LangTable* lang)
{
	if (!h)
		return false;
	hud_drag_close(h);
	char b[256];
	struct HudDrag* d = (struct HudDrag*)calloc(1, sizeof(struct HudDrag));
	if (!d)
		return false;
	u32 want[DE_COUNT];
	for (int e = 0; e < DE_COUNT; e++)
		want[e] = kDragName[e] ? presetcar_hash(kDragName[e]) : HUD_COUNTDOWN_HASH;
	const int nWidgets = readWidgetsInto(d->w, DE_COUNT, want, fng, fngLen, lang);
	if (nWidgets <= 0) {
		h->log("hud: HUD_Drag.fng did not parse -- the drag keeps the race HUD");
		free(d);
		return false;
	}
	Nfsh n;
	if (!nfshOpen(&n, nfshPath, h->log)) {
		free(d);
		return false;
	}

	// ChooseLoadableTextures' drag arm (0x80225918): the plate and the
	// redline band by GetTachStyle; the rest keep the art the .fng authors.
	const int ts = tachStyle(eng->redlineRpm, eng->limiterRpm);
	static const int kDragPlateRpm[8] = { 10000, 10000, 9000, 9000, 8000, 8000, 7000, 7000 };
	char name[48];
	u32 tex[DE_IMAGES];
	for (int e = 0; e < DE_IMAGES; e++)
		tex[e] = d->w[e].resource;
	snprintf(name, sizeof(name), "DRAG_RPM_%d_LINES", kDragPlateRpm[ts]);
	tex[DE_PLATE] = presetcar_hash(name);
	snprintf(name, sizeof(name), "DRAG_RPM_REDLINES_%d", 9000 - 500 * ts);
	tex[DE_REDLINE] = presetcar_hash(name);
	int loaded = 0;
	for (int e = 0; e < DE_IMAGES; e++) {
		u16 fmt = 0;
		u8* px = d->w[e].found && tex[e] ? nfshTexture(&n, tex[e], &d->tex[e], &d->sub[e], &fmt) : NULL;
		if (px) {
			d->texValid[e] = true;
			loaded++;
			free(px);
		}
	}
	nfshClose(&n);
	// The constructor's GetTextureInfo on the plate: none, and Update hides
	// the plate and the redline band every frame.
	d->plateMissing = !d->texValid[DE_PLATE];

	d->angStart = angStart;
	d->angEnd = angEnd;
	d->redline = eng->redlineRpm;
	d->limiter = eng->limiterRpm > 0.0f ? eng->limiterRpm : 1.0f;
	d->gearBase = widgetArgb(&d->w[DE_GEAR]);
	d->needleBase = widgetArgb(&d->w[DE_NEEDLE]);
	d->needleColor = d->needleBase;
	d->lastGear = 1;
	// INIT on Shift_light chains to PulseGreen as the package goes up.
	d->lightScript = DL_GREEN;
	d->lightMs = osGetTime();
	langCopy(lang, 0x0598B065u, "Perfect Shift!", d->sPerfect, sizeof(d->sPerfect));
	langCopy(lang, 0x27D2DD45u, "Good Shift!", d->sGood, sizeof(d->sGood));
	langCopy(lang, 0x75941DC1u, "Missed Shift!", d->sMissed, sizeof(d->sMissed));
	langCopy(lang, 0x2303D76Cu, "Short Shift!", d->sShort, sizeof(d->sShort));
	langCopy(lang, 0x27D75CD8u, "Over Rev!", d->sOver, sizeof(d->sOver));
	d->msgText = d->sPerfect;

	// Furthest first, the engine's draw order.
	for (int e = 0; e < DE_COUNT; e++) {
		if (!d->w[e].found)
			continue;
		int j = d->orderCount++;
		while (j > 0 && d->w[d->order[j - 1]].depth < d->w[e].depth) {
			d->order[j] = d->order[j - 1];
			j--;
		}
		d->order[j] = e;
	}
	h->drag = d;

	snprintf(b, sizeof(b), "hud: drag package -- %d/%d widgets, %d/%d textures, tach style "
		"%d (plate %d%s, redline %d), needle %.0f..%.0f deg", nWidgets, DE_COUNT, loaded,
		DE_IMAGES, ts, kDragPlateRpm[ts], d->plateMissing ? " MISSING" : "", 9000 - 500 * ts,
		angStart, angEnd);
	h->log(b);
	return true;
}

// CalcAngleForRPMDrag (0x8010D000), the one-player sweep: the fraction of the
// limiter across start..end, the short way round unless end is not past start.
static float calcAngleForRpmDrag(float rpm, float limiter, float start, float end) {
	float t = rpm / limiter;
	if (!(t >= 0.0f))
		t = 0.0f;
	else if (t > 1.0f)
		t = 1.0f;
	const float span = end <= start ? 360.0f - start + end : end - start;
	float a = t * span + start;
	if (a > 360.0f)
		a -= 360.0f;
	return a >= -360.0f ? a : a + 360.0f;
}

static bool dragZoomRunning(const struct HudDrag* d, u64 nowMs) {
	return d->msgZoom && (float)(nowMs - d->msgMs) < HUD_DRAG_ZOOM_MS;
}

// FEngSetScript(ShiftMessage, "ZoomIn") with FEngSetVisible.
static void dragMessage(struct HudDrag* d, const char* text, u64 nowMs) {
	d->msgText = text;
	d->msgZoom = true;
	d->msgMs = nowMs;
	d->msgVisible = true;
}

// A shown message's flag comes down, and the string goes, once ZoomIn has
// run out. Retail keeps the flag while Player +0x58 is 6 or 7, a player state
// no race the port runs is in.
static void dragMessageDone(struct HudDrag* d, bool* flag, u64 nowMs) {
	if (*flag && !dragZoomRunning(d, nowMs)) {
		*flag = false;
		d->msgVisible = false;
	}
}

static bool dragPulseRunning(const struct HudDrag* d, int script, u64 nowMs) {
	return d->lightScript == script && (float)(nowMs - d->lightMs) < HUD_DRAG_PULSE_MS;
}

static void dragPulse(struct HudDrag* d, int script, u64 nowMs) {
	if (!dragPulseRunning(d, script, nowMs)) {
		d->lightScript = script;
		d->lightMs = nowMs;
	}
}

// DragTachometer::Update (0x8010C458) past its feature-mask test: the redline
// band, the shift messages and the shift light, off the DriveTrain's shift
// bonus for the gear the tach showed the frame before. `gear` is Car +0x444,
// the engine's numbering.
static void dragTachUpdate(struct HudDrag* d, const HudFrame* f, u64 nowMs) {
	const CarSim* s = f->sim;
	const int gear = f->gear + 1;
	const float rpm = f->rpm;
	const float bonus = carsim_shift_bonus(s);

	// RPM_Redline blinks while the last shift over-revved, the Over Rev!
	// message has not been up, and the engine is past its redline.
	if (bonus >= 0.0f || d->overShown || rpm <= d->redline) {
		if (d->blinking)
			d->blinking = false;
	} else if (!d->blinking) {
		d->blinking = true;
		d->blinkMs = nowMs;
	}

	// Perfect / Good Shift! for a scored shift into a new gear.
	if (bonus > 0.0f && bonus <= 1.0f && !d->msgUp && gear != d->lastGear) {
		dragMessage(d, bonus > 0.5f ? d->sPerfect : d->sGood, nowMs);
		d->msgUp = true;
	} else {
		dragMessageDone(d, &d->msgUp, nowMs);
	}
	// Missed Shift! is IsGrindingGears (DriveTrain +0x5C past its limit); the
	// port's DriveTrain never grinds, so only the clearing half runs.
	dragMessageDone(d, &d->grindUp, nowMs);
	// Short Shift! / Over Rev! on an upshift while racing that earned nothing.
	const bool racing = f->race && f->race->stage == RACE_STAGE_RACING;
	if (racing && d->lastGear < gear && d->lastGear != 0 && !d->msgUp && !d->grindUp) {
		if (bonus == 0.0f && !d->shortShown) {
			dragMessage(d, d->sShort, nowMs);
			d->shortShown = true;
		} else if (bonus < 0.0f && !d->overShown) {
			dragMessage(d, d->sOver, nowMs);
			d->overShown = true;
		}
	} else {
		dragMessageDone(d, &d->shortShown, nowMs);
		dragMessageDone(d, &d->overShown, nowMs);
	}

	// Shift_light and the needle's colour, for the gear the tach showed.
	float range = carsim_perfect_shift_range(s, d->lastGear);
	const float pot = carsim_shift_potential_gear(s, rpm, d->lastGear);
	if (range < HUD_DRAG_MIN_RANGE)
		range = HUD_DRAG_MIN_RANGE;
	const float opt = carsim_optimal_shift_rpm_gear(s, d->lastGear);
	if ((pot == 0.0f && rpm < opt - range) || s->shifting) {
		d->lightVisible = false;
		d->needleColor = d->needleBase;
		return;
	}
	if (pot == -1.0f) {
		d->needleColor = HUD_DRAG_NEEDLE_OVER;
		dragPulse(d, DL_RED, nowMs);
	} else if (pot <= 0.5f) {
		if (gear != 0) {
			// Blend: each channel from the needle's own toward the blue,
			// clamped to 0..255, the fraction left unclamped.
			const float t = (rpm - (opt - range)) / (range * 0.5f);
			const u32 base = d->needleBase;
			const float from[4] = { (float)(base >> 24), (float)((base >> 16) & 0xFF),
				(float)((base >> 8) & 0xFF), (float)(base & 0xFF) };
			u32 c = 0;
			for (int i = 0; i < 4; i++) {
				float v = t * (kDragNeedleBlue[i] - from[i]) + from[i];
				v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
				c = c << 8 | (u32)(int)v;
			}
			dragPulse(d, DL_BLUE, nowMs);
			d->needleColor = c;
		}
	} else {
		d->needleColor = HUD_DRAG_NEEDLE_GOOD;
		dragPulse(d, DL_GREEN, nowMs);
	}
	d->lightVisible = true;
}

// One of the package's images at its authored place, turned `angleDeg` about
// its centre placed at canvas (cx, cy).
static void dragImageAt(const struct HudDrag* d, int e, float cx, float cy, float angleDeg,
	u32 color)
{
	if (!d->texValid[e])
		return;
	const HudWidget* w = &d->w[e];
	const Tex3DS_SubTexture* sub = &d->sub[e];
	C2D_Image img = { (C3D_Tex*)&d->tex[e], sub };
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, color, 1.0f);
	C2D_DrawImageAtRotated(img, dragOriginX(e) + cx * HUD_SCALE, screenY(cy), 0.5f,
		angleDeg * (float)M_PI / 180.0f, &tint, w->w * HUD_SCALE / (float)sub->width,
		w->h * HUD_SCALE / (float)sub->height);
}

// SetVerticalBarValue (0x80107F28): `value` of `max` puts the art's top V
// between (H - lo) / H and hi / H, H the art's height (each bar's authored
// 256), and the image keeps its bottom edge, showing the art from that V
// down. A V below 0 reaches past the art's top, which clamps to its edge.
static void dragVerticalBar(const struct HudDrag* d, int e, float value, float max, float lo,
	float hi, u32 color)
{
	if (!d->texValid[e])
		return;
	const HudWidget* w = &d->w[e];
	const float H = w->h;
	float t = value / max;
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	const float v0 = (H - lo) / H, v1 = hi / H;
	const float v = v0 + (v1 - v0) * t;
	// citro2d's t runs bottom-up: the art's top row is 1 (loadTexture).
	Tex3DS_SubTexture sub = d->sub[e];
	sub.top = 1.0f - v;
	const float drawH = (1.0f - v) * H * HUD_SCALE;
	if (drawH <= 0.0f)
		return;
	C2D_Image img = { (C3D_Tex*)&d->tex[e], &sub };
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, color, 1.0f);
	const float bottom = screenY(w->y + H * 0.5f);
	const float left = dragOriginX(e) + (w->x - w->w * 0.5f) * HUD_SCALE;
	C2D_DrawImageAt(img, left, bottom - drawH, 0.5f, &tint, w->w * HUD_SCALE / (float)sub.width,
		drawH / (float)sub.height);
}

static void dragText(Hud* h, int e, const char* str, u32 color, C2D_Font font, float fontUnitPx,
	float scale, float dy)
{
	drawText(h, &h->drag->w[e], dragOriginX(e), str, color, font, fontUnitPx, scale, 0.0f, dy);
}

// The drag package's frame: the elements' updates, then its objects
// furthest first. The countdown and race-over strings are hud_draw's.
static void dragDraw(Hud* h, const HudFrame* f, u64 nowMs, C2D_Font font, float fontUnitPx,
	const char* cdText, int cdAlpha, float cdScale, const char* roText, float roMs)
{
	struct HudDrag* d = h->drag;
	if (f->gauges && f->sim)
		dragTachUpdate(d, f, nowMs);
	d->lastGear = f->gear + 1;

	// The speed and units, as the cluster prints them ("%3d", 0x80326330).
	char speed[16], gear[2] = { gearLetter(f->gear), 0 };
	snprintf(speed, sizeof(speed), "%3d",
		(int)(fabsf(f->speedMs) * (h->kph ? HUD_MS_TO_KPH : HUD_MS_TO_MPH)));
	const float needle = calcAngleForRpmDrag(f->rpm, d->limiter, d->angStart, d->angEnd);
	const u32 gearColor = f->clutchOut ? HUD_DRAG_GEAR_SHIFTING : d->gearBase;
	const float heat = f->sim ? carsim_engine_heat(f->sim) : 0.0f;
	const float nos = f->nos;

	for (int k = 0; k < d->orderCount; k++) {
		const int e = d->order[k];
		const HudWidget* w = &d->w[e];
		if ((isDragGaugeElem(e) && !f->gauges) ||
				((e == DE_SP_TYPE || e == DE_SP_POINT) && !f->score) ||
				((e == DE_TIME_BACKING || e == DE_RACE_TIME) && !f->racePos) ||
				isDragBottomElem(e))
			continue;
		if ((e == DE_PLATE || e == DE_REDLINE) && d->plateMissing)
			continue;
		C2D_SetTintMode(e < DE_IMAGES ? C2D_TintMult : C2D_TintSolid);
		switch (e) {
		case DE_REDLINE:
			if (d->blinking) {
				const float ms = fmodf((float)(nowMs - d->blinkMs), HUD_DRAG_BLINK_MS);
				const int r = (int)scriptTrack(kDragBlinkMs, kDragBlinkRed, 8, ms);
				dragImageAt(d, e, w->x, w->y, 0.0f, C2D_Color32((u8)r, 0, 0, HUD_DRAG_BLINK_ALPHA));
			} else {
				dragImageAt(d, e, w->x, w->y, 0.0f, w->color);
			}
			break;
		case DE_NEEDLE: {
			const float a = needle * (float)M_PI / 180.0f;
			const float px = w->x + HUD_DRAG_PIVOT_DX, py = w->y;
			dragImageAt(d, e, px - HUD_DRAG_PIVOT_DX * cosf(a), py - HUD_DRAG_PIVOT_DX * sinf(a),
				needle, argbToC2d(d->needleColor));
			break;
		}
		case DE_SHIFT:
			if (d->lightVisible) {
				const float ms = (float)(nowMs - d->lightMs);
				const int a = (int)scriptTrack(kDragPulseMs, kDragPulseAlpha, 4, ms);
				if (a > 0)
					dragImageAt(d, e, w->x, w->y, 0.0f,
						argbToC2d((u32)a << 24 | kDragPulseRgb[d->lightScript]));
			}
			break;
		case DE_HEAT:
			dragVerticalBar(d, e, heat, 1.0f, HUD_DRAG_HEAT_LO, HUD_DRAG_HEAT_HI, w->color);
			break;
		case DE_NOS_LINE:
			// NitrousGauge's one-player drag arm: up to a full bottle the line
			// shows what is left, past it the line is full and the
			// gamebreaker shows the overfill; an empty one hides both.
			if (nos > 0.0f)
				dragVerticalBar(d, e, nos > 1.0f ? 1.0f : nos, 1.0f, HUD_DRAG_NOS_LO,
					HUD_DRAG_NOS_HI, w->color);
			break;
		case DE_NOS_GB:
			if (nos > 1.0f)
				dragVerticalBar(d, e, nos - 1.0f, 1.0f, HUD_DRAG_NOS_LO, HUD_DRAG_NOS_HI, w->color);
			break;
		case DE_RACE_OVER_BACKING:
			if (roText[0]) {
				const int a = (int)scriptTrack(kRoKeyMs, kRoBackAlpha, 4, roMs);
				const float bw = HUD_RO_BACK_W * HUD_SCALE, bh = HUD_RO_BACK_H * HUD_SCALE;
				C2D_DrawRectSolid(HUD_CENTRE_X - bw * 0.5f, screenY(0.0f) - bh * 0.5f, 0.5f, bw, bh,
					C2D_Color32(0, 0, 0, (u8)a));
			}
			break;
		case DE_GEAR:
			dragText(h, e, gear, argbToC2d(gearColor), font, fontUnitPx, 1.0f, 0.0f);
			break;
		case DE_SHIFT_MSG:
			if (d->msgVisible && dragZoomRunning(d, nowMs)) {
				const float ms = (float)(nowMs - d->msgMs);
				const int a = (int)scriptTrack(kDragZoomMs, kDragZoomAlpha, 4, ms);
				const float size = scriptTrack(kDragZoomSizeMs, kDragZoomSize, 2, ms);
				dragText(h, e, d->msgText, C2D_Color32(HUD_DRAG_ZOOM_GREY, HUD_DRAG_ZOOM_GREY,
					HUD_DRAG_ZOOM_GREY, (u8)a), font, fontUnitPx, size / (w->h > 0.0f ? w->h : 1.0f),
					0.0f);
			}
			break;
		case DE_SPEED:
			dragText(h, e, speed, w->color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case DE_UNITS:
			dragText(h, e, h->unitText, w->color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case DE_RACE_TIME: {
			// RacePosition::Update: +0x50 through PrintToString(.., 0).
			char t[16];
			race_print_time(h->rpTime, 0, t);
			dragText(h, e, t, w->color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		}
		case DE_COUNTDOWN:
			if (cdText[0])
				dragText(h, e, cdText, C2D_Color32(w->rgba[0], w->rgba[1], w->rgba[2], (u8)cdAlpha),
					font, fontUnitPx, cdScale, 0.0f);
			break;
		case DE_RACE_OVER:
			if (roText[0]) {
				const int a = (int)scriptTrack(kRoKeyMs, kRoMsgAlpha, 4, roMs);
				const float sc = scriptTrack(kRoKeyMs, kRoMsgScale, 4, roMs);
				dragText(h, e, roText, C2D_Color32(HUD_RO_MSG_GREY, HUD_RO_MSG_GREY,
					HUD_RO_MSG_GREY, (u8)a), font, fontUnitPx, sc, HUD_DRAG_RO_MSG_Y - w->y);
			}
			break;
		case DE_SP_TYPE:
			stuntDrawText(h, w, dragOriginX(e), &h->spName, f->worldNow, font, fontUnitPx);
			break;
		case DE_SP_POINT:
			stuntDrawText(h, w, dragOriginX(e), &h->spPoints, f->worldNow, font, fontUnitPx);
			break;
		default:
			dragImageAt(d, e, w->x, w->y, 0.0f, w->color);
			break;
		}
	}
	C2D_SetTintMode(C2D_TintSolid);
}

// ---- the drift race's package, HUD_Drift.fng (TASKS.md #268 E) -------------
// What FEngHud's constructor builds over InGameDrift.bun's screen in a drift:
// the single race's cluster at HUD_Drift's own places; DriftGauge
// (__10DriftGaugePCci 0x8010D6CC, Update 0x8010DABC) -- the drift angle, Too
// Slow!, the wrong-way countdown and the King drift names; and
// CunningStuntSpread's drift arm (UpdateDriftStunts 0x8010F780) -- the
// running drift's points, the best drift, the multiplier with the points the
// next one needs, and the total. The leaderboard by points is the bottom
// screen's (hud_draw_race_stats).

typedef enum {
	// Images first: their index is also their texture slot.
	DR_BACKING1, DR_BACKING3, DR_BEST_BACKING, DR_TOTAL_BACKING,
	DR_IMAGES,
	// White16x16 tinted: drawn as a solid rectangle.
	DR_TRAP_BACKING = DR_IMAGES,
	DR_ANGLE, DR_ANGLE_DEG, DR_SLOW, DR_WW_COUNT, DR_COMMENT, DR_SP_TITLE, DR_SP_POINT,
	DR_BEST_TEXT, DR_BEST, DR_MULT, DR_TRAP, DR_NEXT, DR_TOTAL, DR_TOTAL_TITLE,
	DR_HURRY_TEXT, DR_HURRY_COUNT,
	DR_COUNT
} HudDriftElem;

static const char* const kDriftName[DR_COUNT] = {
	"Drift_Angle_Backing1",         // DriftGauge, registered plain with a turbo
	"Drift_Angle_Backing3",         // and without one
	"BestDriftBacking",             // CunningStuntSpread, registered plain
	"Style_Point_Backing_03",       // registered plain
	"CurrentSpeed_Trap_Backing_01", // registered plain
	"Angle_Indicator_DATA",         // DriftGauge +0x64
	"Angle_degrees",                // registered plain
	"SlowMessage",                  // +0x6C
	"WrongWay_CountDown",           // +0x70
	"Comment_DATA",                 // +0x74
	"SP_Drift_Title",               // CunningStuntSpread's name string
	"SP_Drift_Point",               // and its points
	"BestLapTimeText",              // registered plain
	"BestDrift",                    // +0x3C
	"drift_multiplier_text",        // +0x50
	"SpeedTrap_Speed_DATA",         // +0x54
	"NextSpeeed_Trap_Indicator",    // +0x4C
	"SP_DRIFT_TOTAL",               // +0x40
	"Style_TotalDrift_Title",       // +0x44
	"Hurry_Text",                   // RacePosition +0x40, its QueuedMessageSwapper's
	"Hurry_Counter",                // RacePosition +0x44
};

// DriftGauge's (its feature bits are the Gauges row's); the rest are
// CunningStuntSpread's (the Score row's).
static bool isDriftGaugeElem(int e) {
	return e == DR_BACKING1 || e == DR_BACKING3 || (e >= DR_ANGLE && e <= DR_COMMENT);
}

// Where canvas x = 0 lands, at the cluster's half scale: the angle meter and
// Too Slow! keep the cluster's right edge, the strings authored on the
// centre line go on the screen's middle, and the score block, authored
// against the canvas's left edge, on the screen's -- the drag's mapping.
static float driftOriginX(int e) {
	switch (e) {
	case DR_BACKING1: case DR_BACKING3: case DR_ANGLE: case DR_ANGLE_DEG: case DR_SLOW:
		return HUD_ORIGIN_X;
	case DR_WW_COUNT: case DR_COMMENT: case DR_SP_TITLE: case DR_SP_POINT:
	case DR_HURRY_TEXT: case DR_HURRY_COUNT:
		return HUD_CENTRE_X;
	default:
		return HUD_DRAG_LEFT_X;
	}
}

// 0xAARRGGBB: UpdateBonusZone's points colour in a bonus zone or past
// traffic (else the constructor's HUD_SP_GREEN), and UpdateDriftStunts'
// next-level figure, steady, or blinking with the green as the drift nears it.
#define HUD_DR_BONUS 0xFFFF8020u
#define HUD_DR_NEXT  0xFFAA8000u
// UpdateBonusZone's test (0x80326914): either multiplier above 1.
#define HUD_DR_BONUS_MIN 1.0f
// UpdateDriftGauge: the angle shown is at most 90 degrees (0x803268F4).
#define HUD_DR_ANGLE_MAX 90.0f
// UpdateDriftMessage: a King drift's name stays up 2 s (0x803268EC).
#define HUD_DR_NIFTY_SEC 2.0f
// UpdateDriftStunts: an aborted drift's red points stay up 1 s (0x80326DC4);
// the next level's figure blinks every next / points x 0.05 s (0x80326D78),
// at most 0.5 s (0x80326DC8).
#define HUD_DR_ABORT_SEC  1.0f
#define HUD_DR_BLINK_STEP 0.05f
#define HUD_DR_BLINK_MAX  0.5f

// The package's scripts, decoded out of HUD_Drift.fng (scrdump), all linear;
// colours as RGB.
//   SlowMessage   TooSlowToGain 0xD20D8778, 400 ms, loops: (202,244,138) at
//                 alpha 200 to 180 ms, 0 from 200. LosingMult 0xE803718D,
//                 400 ms, loops: the same at 255 to 200 ms, 0 from 220.
//   SP_Drift_Point, WrongWay_CountDown   INIT chains to Pulse 0x03826A28,
//                 600 ms, loops: size 1.0 -> 0.8 -> 1.0 and 0.7 -> 0.5 -> 0.7.
//   drift_multiplier_text   INIT (48,207,48) at 0.9; Highlight 0x249DB7B7,
//                 600 ms then INIT: green 207 -> 249, alpha 255 -> 0, size
//                 0.9 -> 1.2.
//   SP_DRIFT_TOTAL, Style_TotalDrift_Title   INIT (207,233,242) at 200 and
//                 (192,192,192); Highlight, 400 ms then INIT: (48,207,48)
//                 and (143,430,16) -- green clamped -- in turn, 100 ms each
//                 with 20 ms between.
//   Angle_Indicator_DATA, Angle_degrees   NoTurbo 0x466443C8 moves them 73
//                 right; Turbo 0x03CAE38B leaves them.
#define HUD_DR_SLOW_MS 400.0f
static const float kDrTooSlowMs[4] = { 0.0f, 180.0f, 200.0f, 400.0f };
static const float kDrTooSlowA[4] = { 200.0f, 200.0f, 0.0f, 0.0f };
static const float kDrLosingMs[4] = { 0.0f, 200.0f, 220.0f, 400.0f };
static const float kDrLosingA[4] = { 255.0f, 255.0f, 0.0f, 0.0f };
#define HUD_DR_SLOW_RGB 0xCAF48Au
#define HUD_DR_PULSE_MS 600.0f
static const float kDrPulseMs[3] = { 0.0f, 300.0f, 600.0f };
static const float kDrPulsePoint[3] = { 1.0f, 0.8f, 1.0f };
static const float kDrPulseCount[3] = { 0.7f, 0.5f, 0.7f };
#define HUD_DR_MULT_HI_MS 600.0f
static const float kDrMultHiMs[2] = { 0.0f, 600.0f };
static const float kDrMultHiG[2] = { 207.0f, 249.0f };
static const float kDrMultHiA[2] = { 255.0f, 0.0f };
static const float kDrMultHiSize[2] = { 0.9f, 1.2f };
#define HUD_DR_MULT_G    207
#define HUD_DR_MULT_SIZE 0.9f
#define HUD_DR_TOTAL_HI_MS 400.0f
static const float kDrTotalHiMs[8] = { 0.0f, 100.0f, 120.0f, 200.0f, 220.0f, 300.0f, 320.0f, 400.0f };
static const float kDrTotalHiT[8] = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f };
#define HUD_DR_NO_TURBO_DX 73.0f

// SlowMessage's script.
enum { DS_SLOW_INIT, DS_SLOW_TOO_SLOW, DS_SLOW_LOSING };

struct HudDrift {
	HudWidget w[DR_COUNT];
	int order[DR_COUNT];
	int orderCount;
	C3D_Tex tex[DR_IMAGES];
	Tex3DS_SubTexture sub[DR_IMAGES];
	bool texValid[DR_IMAGES];
	// The cluster as hud_open read it out of HUD_SingleRace.fng, put back on
	// close.
	HudWidget saved[HE_COUNT];
	bool turbo;              // the constructor's Turbo / NoTurbo choice
	u64 openMs;              // when the INIT -> Pulse chains started

	// DriftGauge.
	char angle[12];
	bool slowVisible;
	int slowScript;          // DS_SLOW_*
	u64 slowMs;
	bool commentUp;          // +0x98, ShowNiftyDriftyMessage has run
	bool commentVisible;
	s32 commentAt;           // +0x9C, WorldTimer
	char comment[32];

	// CunningStuntSpread's drift arm: the DRIFT moment's node, the strings
	// DrawStunt fills, and UpdateDriftStunts' state.
	HudStunt slot;
	u32 pointsColor;         // +0x64, SetDriftPointsColor's
	bool spVisible;
	u32 titleColor, pointColor;
	char title[24], points[16];
	char best[16], total[40], trap[16], next[48], mult[16];
	u32 trapColor;
	int lastLevel;           // +0x84
	bool multHi, totalHi;    // Highlight running on the multiplier, the total
	u64 multHiMs, totalHiMs;
	s32 blinkAt;             // +0x74
	bool blinkOn;            // +0x78

	char sGain[32];          // 0xE03EF7A4
	char sMax[16];           // 0xCC62C8BE

	// RacePosition's drift arm (#464): Hurry_Counter's time left, and
	// Hurry_Text's QueuedMessageSwapper -- "Hurry!" queued as the seconds
	// left reach each even number, up 1.5 s (0x80326114) a time.
	bool hurryOn;
	char hurryCount[16];
	int hurryLast;           // the even second last queued
	u64 hurryMs;             // when that message went up
	char sHurry[24];         // 0x862A0519
};
#define HUD_DR_HURRY_MS 1500.0f

static void driftFree(struct HudDrift* d) {
	if (!d)
		return;
	for (int e = 0; e < DR_IMAGES; e++)
		if (d->texValid[e])
			C3D_TexDelete(&d->tex[e]);
	free(d);
}

void hud_drift_close(Hud* h) {
	if (!h || !h->driftPkg)
		return;
	memcpy(h->w, h->driftPkg->saved, sizeof(h->w));
	hudSortOrder(h, NULL);
	driftFree(h->driftPkg);
	h->driftPkg = NULL;
}

bool hud_drift_open(Hud* h, const char* nfshPath, const u8* fng, u32 fngLen, bool turbo,
	const LangTable* lang)
{
	if (!h)
		return false;
	hud_drift_close(h);
	char b[256];
	struct HudDrift* d = (struct HudDrift*)calloc(1, sizeof(struct HudDrift));
	HudWidget* cl = (HudWidget*)calloc(HE_COUNT, sizeof(HudWidget));
	if (!d || !cl) {
		free(d);
		free(cl);
		return false;
	}
	u32 want[DR_COUNT];
	for (int e = 0; e < DR_COUNT; e++)
		want[e] = presetcar_hash(kDriftName[e]);
	const int nWidgets = readWidgetsInto(d->w, DR_COUNT, want, fng, fngLen, lang);
	u32 cwant[HE_COUNT];
	for (int e = 0; e < HE_COUNT; e++)
		cwant[e] = kWidgetName[e] ? presetcar_hash(kWidgetName[e]) : HUD_COUNTDOWN_HASH;
	const int nCluster = nWidgets > 0 ? readWidgetsInto(cl, HE_COUNT, cwant, fng, fngLen, lang) : 0;
	if (nWidgets <= 0 || nCluster <= 0) {
		h->log("hud: HUD_Drift.fng did not parse -- the drift keeps the race HUD");
		free(cl);
		free(d);
		return false;
	}
	Nfsh n;
	if (!nfshOpen(&n, nfshPath, h->log)) {
		free(cl);
		free(d);
		return false;
	}
	int loaded = 0;
	for (int e = 0; e < DR_IMAGES; e++) {
		if (!d->w[e].found || !d->w[e].resource)
			continue;
		u16 fmt = 0;
		u8* px = nfshTexture(&n, d->w[e].resource, &d->tex[e], &d->sub[e], &fmt);
		d->texValid[e] = px != NULL;
		if (px)
			loaded++;
		free(px);
	}
	nfshClose(&n);

	// The cluster's widgets go to HUD_Drift's places. Their textures stay the
	// ones hud_open loaded -- both screens author the same art -- and so do
	// the car's tints. What this screen has no widget for is not drawn.
	memcpy(d->saved, h->w, sizeof(h->w));
	bool skip[HE_COUNT];
	int moved = 0;
	for (int e = 0; e < HE_COUNT; e++) {
		skip[e] = !cl[e].found;
		if (skip[e] || !h->w[e].found)
			continue;
		const u32 color = h->w[e].color, res = h->w[e].resource;
		h->w[e] = cl[e];
		h->w[e].resource = res;
		if (e == HE_FILL || e == HE_NEEDLE || e == HE_PLATE)
			h->w[e].color = color;
		moved++;
	}
	free(cl);
	hudSortOrder(h, skip);

	for (int e = 0; e < DR_COUNT; e++) {
		if (!d->w[e].found)
			continue;
		int j = d->orderCount++;
		while (j > 0 && d->w[d->order[j - 1]].depth < d->w[e].depth) {
			d->order[j] = d->order[j - 1];
			j--;
		}
		d->order[j] = e;
	}

	// The constructors' strings: SlowMessage's language hash, the next
	// level's line and its top.
	langCopy(lang, 0xB8387A0Bu, "Too Slow!", d->w[DR_SLOW].text, sizeof(d->w[DR_SLOW].text));
	langCopy(lang, 0xE03EF7A4u, "Gain %dX with:", d->sGain, sizeof(d->sGain));
	langCopy(lang, 0xCC62C8BEu, "MAX", d->sMax, sizeof(d->sMax));
	langCopy(lang, 0x862A0519u, "Hurry!", d->sHurry, sizeof(d->sHurry));
	d->hurryLast = -1;
	d->turbo = turbo;
	d->openMs = osGetTime();
	d->pointsColor = HUD_SP_GREEN;
	d->trapColor = HUD_DR_NEXT;
	// The constructor's UpdateHypoMultiplier(0): "X0", +0x84 = 0. The total
	// keeps the screen's own text until the first drift is banked.
	snprintf(d->mult, sizeof(d->mult), "X0");
	snprintf(d->total, sizeof(d->total), "%s", d->w[DR_TOTAL].text);
	h->driftPkg = d;

	snprintf(b, sizeof(b), "hud: drift package -- %d/%d widgets, %d/%d textures, %d cluster "
		"widgets moved (%d drawn), %s, total starts as \"%s\"", nWidgets, DR_COUNT, loaded,
		DR_IMAGES, moved, h->orderCount, turbo ? "turbo" : "no turbo", d->total);
	h->log(b);
	return true;
}

// "Gain %dX with:" with `n` in place of the %d, the language string never
// used as a format itself.
static void driftGainText(const char* fmt, int n, char* out, size_t cap) {
	const char* p = strstr(fmt, "%d");
	if (!p) {
		snprintf(out, cap, "%s", fmt);
		return;
	}
	snprintf(out, cap, "%.*s%d%s", (int)(p - fmt), fmt, n, p + 2);
}

// UpdateHypoMultiplier (0x8010F670): Highlight when the level went up, then
// "X%$d".
static void driftHypoMult(struct HudDrift* d, int level, u64 nowMs) {
	if (d->lastLevel < level) {
		d->multHi = true;
		d->multHiMs = nowMs;
	}
	char g[12];
	stuntGrouped(level, g, sizeof(g));
	snprintf(d->mult, sizeof(d->mult), "X%s", g);
	d->lastLevel = level;
}

// One frame of the package's elements: DriftGauge::Update on the Gauges row,
// then UpdateStylePoints' drift arm and UpdateDriftStunts on the Score row.
static void driftUpdate(struct HudDrift* d, const HudFrame* f, u64 nowMs) {
	const HudDriftFrame* in = &f->dr;
	const s32 now = f->worldNow;
	// RacePosition::Update's drift arm (0x801083F8, #464): Hurry_Counter
	// shows the timer's time left through Timer::PrintToString(.., 0); as
	// the time left comes within a frame above an even second, "Hurry!" is
	// queued on Hurry_Text's swapper, which holds each message 1.5 s.
	// Off again once it runs out: the player is finished at that moment
	// (Race::Update), as DriftGauge::UpdateCountdown drops its own counter.
	d->hurryOn = in->hurryLeft > 0.0f;
	if (d->hurryOn) {
		race_print_time((s32)(in->hurryLeft * 4000.0f), 0, d->hurryCount);
		const int whole = (int)in->hurryLeft;
		if ((whole & 1) == 0 && whole != d->hurryLast && in->hurryLeft - (float)whole < 0.1f) {
			d->hurryLast = whole;
			d->hurryMs = nowMs;
		}
	}
	if (d->multHi && (float)(nowMs - d->multHiMs) >= HUD_DR_MULT_HI_MS)
		d->multHi = false;
	if (d->totalHi && (float)(nowMs - d->totalHiMs) >= HUD_DR_TOTAL_HI_MS)
		d->totalHi = false;

	if (f->gauges) {
		// UpdateDriftGauge: none in reverse; at most 90.
		float a = f->gear < 0 ? 0.0f : fabsf(in->yawDeg);
		if (a > HUD_DR_ANGLE_MAX)
			a = HUD_DR_ANGLE_MAX;
		snprintf(d->angle, sizeof(d->angle), "%d", (int)a);
		// UpdateSlowMessage: LosingMult first, then TooSlowToGain; each
		// script set once and left looping.
		d->slowVisible = in->losing || in->tooSlow;
		if (d->slowVisible) {
			const int want = in->losing ? DS_SLOW_LOSING : DS_SLOW_TOO_SLOW;
			if (d->slowScript != want) {
				d->slowScript = want;
				d->slowMs = nowMs;
			}
		}
		// UpdateBonusZone -> SetDriftPointsColor.
		d->pointsColor = in->bonus > HUD_DR_BONUS_MIN || in->dodge > HUD_DR_BONUS_MIN ?
			HUD_DR_BONUS : HUD_SP_GREEN;
		// ShowNiftyDriftyMessage, then UpdateDriftMessage.
		if (in->nifty) {
			d->commentUp = true;
			d->commentAt = now;
			d->commentVisible = true;
			snprintf(d->comment, sizeof(d->comment), "%s", in->nifty);
		}
		if (!d->commentUp || (float)(now - d->commentAt) * 0.00025f > HUD_DR_NIFTY_SEC)
			d->commentVisible = false;
	}
	if (!f->score)
		return;

	// UpdateStylePoints (0x8010FFD0), its drift arm: the DRIFT moment, each
	// frame it is listed, even at 0 points. An aborted one keeps the points
	// it had, and takes the King drift's name down.
	HudStunt* s = &d->slot;
	for (int i = 0; i < f->stuntCount; i++) {
		const StyleUp* m = &f->stunts[i];
		if (m->type != STYLE_DRIFT)
			continue;
		s->continuous = m->continuous;
		s->aborted = m->state == STYLE_STATE_ABORTED;
		if (s->aborted)
			d->commentVisible = false;
		else
			s->points = m->points;
		s->mult = (int)(m->mult + 0.5f);
		s->type = m->type;
		s->shownAt = 0;
		s->nameHash = m->nameHash;
		s->state = m->state;
		s->drawn = 1;
		snprintf(s->name, sizeof(s->name), "%s", m->name ? m->name : "");
	}

	// UpdateDriftStunts: the best drift, then the moment's strings -- up the
	// frames it is listed, and an aborted one in red for a second.
	stuntGrouped((int)in->best, d->best, sizeof(d->best));
	d->spVisible = false;
	if (s->drawn) {
		d->spVisible = true;
		// DrawStunt's DRIFT arm: the points whole, no sign or multiplier.
		const bool red = s->aborted || s->points < 0.0f;
		stuntGrouped((int)s->points, d->points, sizeof(d->points));
		snprintf(d->title, sizeof(d->title), "%s", s->name);
		d->titleColor = red ? HUD_SP_RED : HUD_SP_GREEN;
		d->pointColor = red ? HUD_SP_RED : d->pointsColor;
		if (!tickSet(s->shownAt))
			s->shownAt = now;
		if (!s->aborted) {
			s->drawn = 0;
			s->shownAt = 0;
		} else if ((float)(now - s->shownAt) * 0.00025f > HUD_DR_ABORT_SEC) {
			d->spVisible = false;
			s->drawn = 0;
			s->shownAt = 0;
		}
	}
	// A drift banked: the total, highlighted.
	if (in->state == STYLE_STATE_DONE) {
		stuntGrouped((int)in->banked, d->total, sizeof(d->total));
		d->totalHi = true;
		d->totalHiMs = nowMs;
	}
	// The next level: what it needs, blinking faster as this drift nears it;
	// past the top, "----" and MAX.
	driftHypoMult(d, in->level, nowMs);
	if (in->next > 0) {
		stuntGrouped(in->next, d->trap, sizeof(d->trap));
		driftGainText(d->sGain, in->level + 1, d->next, sizeof(d->next));
		if (in->points < (float)in->next) {
			float period = HUD_DR_BLINK_MAX;
			if (in->points > 0.0f) {
				period = (float)in->next / in->points * HUD_DR_BLINK_STEP;
				if (period > HUD_DR_BLINK_MAX)
					period = HUD_DR_BLINK_MAX;
			}
			if ((float)(now - d->blinkAt) * 0.00025f > period) {
				d->blinkOn = !d->blinkOn;
				d->blinkAt = now;
			}
			d->trapColor = d->blinkOn ? HUD_SP_GREEN : HUD_DR_NEXT;
		} else {
			d->trapColor = HUD_DR_NEXT;
		}
	} else {
		snprintf(d->trap, sizeof(d->trap), "----");
		snprintf(d->next, sizeof(d->next), "%s", d->sMax);
		d->trapColor = HUD_DR_NEXT;
	}
}

static void driftText(Hud* h, const struct HudDrift* d, int e, const char* str, u32 color,
	C2D_Font font, float fontUnitPx, float size, float dx)
{
	const HudWidget* w = &d->w[e];
	const float base = w->h > 0.0f ? w->h : 1.0f;
	drawText(h, w, driftOriginX(e), str, color, font, fontUnitPx, size > 0.0f ? size / base : 1.0f,
		dx, 0.0f);
}

static void driftImage(const struct HudDrift* d, int e) {
	if (!d->texValid[e])
		return;
	const HudWidget* w = &d->w[e];
	const Tex3DS_SubTexture* sub = &d->sub[e];
	C2D_Image img = { (C3D_Tex*)&d->tex[e], sub };
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, w->color, 1.0f);
	C2D_DrawImageAtRotated(img, driftOriginX(e) + w->x * HUD_SCALE, screenY(w->y), 0.5f, 0.0f,
		&tint, w->w * HUD_SCALE / (float)sub->width, w->h * HUD_SCALE / (float)sub->height);
}

static u8 driftClamp(float v) {
	return (u8)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
}

// One of the package's objects as its scripts have it now. `size` 0 keeps the
// authored size.
static void driftDrawElem(Hud* h, struct HudDrift* d, int e, const HudFrame* f, u64 nowMs,
	C2D_Font font, float fontUnitPx)
{
	if (e == DR_HURRY_TEXT || e == DR_HURRY_COUNT) {
		if (!f->racePos)   // RacePosition's, on the Position row's bit
			return;
	} else if (isDriftGaugeElem(e) ? !f->gauges : !f->score)
		return;
	const HudWidget* w = &d->w[e];
	const float pulse = fmodf((float)(nowMs - d->openMs), HUD_DR_PULSE_MS);
	C2D_SetTintMode(e < DR_IMAGES ? C2D_TintMult : C2D_TintSolid);
	switch (e) {
	case DR_BACKING1:
	case DR_BACKING3:
		// The constructor registers the one its Turbo / NoTurbo choice uses.
		if ((e == DR_BACKING1) == d->turbo)
			driftImage(d, e);
		break;
	case DR_TRAP_BACKING: {
		const float bw = w->w * HUD_SCALE, bh = w->h * HUD_SCALE;
		C2D_DrawRectSolid(driftOriginX(e) + w->x * HUD_SCALE - bw * 0.5f, screenY(w->y) - bh * 0.5f,
			0.5f, bw, bh, w->color);
		break;
	}
	case DR_ANGLE:
	case DR_ANGLE_DEG:
		driftText(h, d, e, e == DR_ANGLE ? d->angle : w->text, w->color, font, fontUnitPx, 0.0f,
			d->turbo ? 0.0f : HUD_DR_NO_TURBO_DX);
		break;
	case DR_SLOW:
		if (d->slowVisible) {
			const float ms = fmodf((float)(nowMs - d->slowMs), HUD_DR_SLOW_MS);
			const float a = d->slowScript == DS_SLOW_LOSING ? scriptTrack(kDrLosingMs, kDrLosingA, 4, ms)
				: scriptTrack(kDrTooSlowMs, kDrTooSlowA, 4, ms);
			if (a >= 1.0f)
				driftText(h, d, e, w->text, argbToC2d((u32)driftClamp(a) << 24 | HUD_DR_SLOW_RGB), font,
					fontUnitPx, 0.0f, 0.0f);
		}
		break;
	case DR_WW_COUNT:
		// UpdateCountdown: "%$1.1f" of the seconds left, hidden at none.
		if (f->dr.wrongWaySec > 0.0f) {
			char t[16];
			snprintf(t, sizeof(t), "%.1f", f->dr.wrongWaySec);
			driftText(h, d, e, t, w->color, font, fontUnitPx,
				scriptTrack(kDrPulseMs, kDrPulseCount, 3, pulse), 0.0f);
		}
		break;
	case DR_HURRY_COUNT:
		if (d->hurryOn)
			driftText(h, d, e, d->hurryCount, w->color, font, fontUnitPx, 0.0f, 0.0f);
		break;
	case DR_HURRY_TEXT:
		if (d->hurryOn && d->hurryLast >= 0 && (float)(nowMs - d->hurryMs) < HUD_DR_HURRY_MS)
			driftText(h, d, e, d->sHurry, w->color, font, fontUnitPx, 0.0f, 0.0f);
		break;
	case DR_COMMENT:
		if (d->commentVisible)
			driftText(h, d, e, d->comment, w->color, font, fontUnitPx, 0.0f, 0.0f);
		break;
	case DR_SP_TITLE:
		if (d->spVisible)
			driftText(h, d, e, d->title, argbToC2d(d->titleColor), font, fontUnitPx, 0.0f, 0.0f);
		break;
	case DR_SP_POINT:
		if (d->spVisible)
			driftText(h, d, e, d->points, argbToC2d(d->pointColor), font, fontUnitPx,
				scriptTrack(kDrPulseMs, kDrPulsePoint, 3, pulse), 0.0f);
		break;
	case DR_BEST_TEXT:
		driftText(h, d, e, w->text, w->color, font, fontUnitPx, 0.0f, 0.0f);
		break;
	case DR_BEST:
		driftText(h, d, e, d->best, w->color, font, fontUnitPx, 0.0f, 0.0f);
		break;
	case DR_MULT: {
		float g = HUD_DR_MULT_G, a = 255.0f, size = HUD_DR_MULT_SIZE;
		if (d->multHi) {
			const float ms = (float)(nowMs - d->multHiMs);
			g = scriptTrack(kDrMultHiMs, kDrMultHiG, 2, ms);
			a = scriptTrack(kDrMultHiMs, kDrMultHiA, 2, ms);
			size = scriptTrack(kDrMultHiMs, kDrMultHiSize, 2, ms);
		}
		if (a >= 1.0f)
			driftText(h, d, e, d->mult, C2D_Color32(48, driftClamp(g), 48, driftClamp(a)), font,
				fontUnitPx, size, 0.0f);
		break;
	}
	case DR_TRAP:
		driftText(h, d, e, d->trap, argbToC2d(d->trapColor), font, fontUnitPx, 0.0f, 0.0f);
		break;
	case DR_NEXT:
		driftText(h, d, e, d->next, w->color, font, fontUnitPx, 0.0f, 0.0f);
		break;
	case DR_TOTAL:
	case DR_TOTAL_TITLE: {
		u32 c = e == DR_TOTAL ? C2D_Color32(207, 233, 242, 200) : C2D_Color32(192, 192, 192, 255);
		if (d->totalHi) {
			const float t = scriptTrack(kDrTotalHiMs, kDrTotalHiT, 8, (float)(nowMs - d->totalHiMs));
			c = C2D_Color32(driftClamp(48.0f + 95.0f * t), driftClamp(207.0f + 223.0f * t),
				driftClamp(48.0f - 32.0f * t), 255);
		}
		driftText(h, d, e, e == DR_TOTAL ? d->total : w->text, c, font, fontUnitPx, 0.0f, 0.0f);
		break;
	}
	default:
		driftImage(d, e);
		break;
	}
}

// FEngSetLanguageHash, FEngSetVisible and FEngSetScript("ZoomIn") on
// Out_Run_Message: a new line restarts the script.
static void gaMessage(Hud* h, const char* text, u64 nowMs) {
	snprintf(h->orText, sizeof(h->orText), "%s", text);
	h->orShown = true;
	h->orStartMs = nowMs;
}

// SetHorizontalBarValue (0x8010805C) on Getaway_Distance_Bar: `value` (of 1)
// puts the art's right U between (W - lo) / W and 1 - hi / W, W the art's
// authored 256, and the image keeps its left edge -- so the bar shows the art
// from its left edge to pixel 99 at 0 and to 255 at 1. The art is clear up
// to pixel 99, which is the empty meter.
static void gaDrawBar(Hud* h, float value, u32 argb) {
	const int e = HE_GA_BAR;
	if (!h->texValid[e])
		return;
	const HudWidget* w = &h->w[e];
	const float W = w->w;
	const float t = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
	const float u0 = (W - HUD_GA_BAR_LO) / W, u1 = HUD_GA_BAR_HI / W;
	const float shown = 1.0f - (u0 + (u1 - u0) * t);
	if (shown <= 0.0f)
		return;
	Tex3DS_SubTexture sub = h->sub[e];
	sub.right = sub.left + (sub.right - sub.left) * shown;
	C2D_Image img = { &h->tex[e], &sub };
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, argbToC2d(argb), 1.0f);
	C2D_DrawImageAt(img, screenX(e, w->x - W * 0.5f), screenY(w->y - w->h * 0.5f), 0.5f, &tint,
		shown * W * HUD_SCALE / (float)sub.width, w->h * HUD_SCALE / (float)sub.height);
}

static void splitTimeUpdate(Hud* h, const HudFrame* f);

void hud_draw(Hud* h, const HudFrame* f, C2D_Font font, float fontUnitPx) {
	if (!h)
		return;
	C2D_TextBufClear(h->buf);
	const u64 nowMs = osGetTime();
	wrongWayUpdate(h, f->wrongWay, nowMs);

	// ShowcaseRewardIndicator::Update (SpeedPS.elf 0x80108BB0), which only
	// runs while the HUD is up -- a shoot's preview and magazine modes take
	// it down, so the line QueueMessage left waits for the street to be back:
	// its frames counted down, then the text printed and ZoomIn started.
	if (h->crQueued && h->crDelay > 0)
		h->crDelay--;
	if (h->crQueued && h->crDelay == 0) {
		h->crQueued = false;
		h->crShown = true;
		h->crStartMs = nowMs;
	}

	// CunningStuntSpread. FEUpdateStylePoints hands it the listed moments only
	// while the HUD's feature mask has 0x20000 (the Score row), and Update
	// runs on the same bit; without it HudElement::Toggle hides the strings.
	// PORT: retail's WorldTimer never goes back; the port's car clock starts
	// again with a new car, and the list goes with it.
	// In a drift its own package takes the list (driftUpdate): Update's drift
	// arm draws the DRIFT moment alone, never the regular list.
	struct HudDrift* dp = h->driftPkg && f->drift ? h->driftPkg : NULL;
	if (f->worldNow < h->spLastNow) {
		stuntReset(h);
		if (h->driftPkg) {
			memset(&h->driftPkg->slot, 0, sizeof(h->driftPkg->slot));
			h->driftPkg->spVisible = h->driftPkg->commentUp = h->driftPkg->commentVisible = false;
			h->driftPkg->blinkAt = 0;
		}
	}
	h->spLastNow = f->worldNow;
	if (dp) {
		driftUpdate(dp, f, nowMs);
	} else if (f->score) {
		stuntUpdateStylePoints(h, f->stunts, f->stuntCount);
		stuntUpdate(h, f->worldNow, f->nosFitted);
	}

	// GetAwayMeter::Update (0x8011252C). Without feature 0x40, Hide: the group
	// InActive and, once the race type is no longer 7, Show's counter back to
	// 0. With it, Show, then the lead d (<= 0 ahead): Distance_Number's
	// "Ahead" / "Behind" pulse and the bar's colour, the lead's change while
	// the race is in stage 4 told by Out_Run_Message, the bar at |d| / D and
	// the number at -d, in metres or (ConvertMetersToFeet) feet.
	const HudGetaway* ga = &f->ga;
	char gaNumber[16] = "";
	const char* gaUnit = "";
	float gaBar = 0.0f;
	u32 gaBarArgb = HUD_GA_AHEAD;
	if (!ga->on) {
		h->gaActive = false;
		h->gaShow = 0;
		h->gaPulse = 0;
	} else {
		// Show (0x801128EC): "Active" once the counter has reached 4; on its
		// way, at 3, Out_Run_Message's ZoomIn with "Outrun Challenge
		// Accepted" -- "Lose Them!" when the player leads, "Catch Them!"
		// when it chases.
		if (h->gaShow == 4)
			h->gaActive = true;
		else if (h->gaShow == 3)
			gaMessage(h, ga->playerFirst ? h->sAcceptLose : h->sAcceptCatch, nowMs);
		if (h->gaShow < 4)
			h->gaShow++;
		const float d = ga->gap;
		if (d <= 0.0f) {
			if (h->gaPulse != 1) {
				h->gaPulse = 1;
				h->gaPulseMs = nowMs;
			}
			gaBarArgb = HUD_GA_AHEAD;
			if (h->gaPrev > 0.0f && ga->racing)
				gaMessage(h, h->sLeadGot, nowMs);
		} else {
			if (h->gaPulse != 2) {
				h->gaPulse = 2;
				h->gaPulseMs = nowMs;
			}
			gaBarArgb = HUD_GA_BEHIND;
			if (h->gaPrev < 0.0f && ga->racing)
				gaMessage(h, h->sLeadLost, nowMs);
		}
		h->gaPrev = d;
		gaBar = ga->dist > 0.0f ? fabsf(d) / ga->dist : 0.0f;
		float lead = -d;
		if (!ga->metric)
			lead *= HUD_M_TO_FT;
		snprintf(gaNumber, sizeof(gaNumber), "%d", (int)lead);
		gaUnit = ga->metric ? h->sMeters : h->sFeet;
	}

	char speed[16], gear[2] = { gearLetter(f->gear), 0 };
	const float v = fabsf(f->speedMs) * (h->kph ? HUD_MS_TO_KPH : HUD_MS_TO_MPH);
	snprintf(speed, sizeof(speed), "%3d", (int)v);
	const float needle = calcAngleForRpm(f->rpm, h->sweep, h->limiter);
	const u32 gearColor = argbToC2d(f->clutchOut ? HUD_GEAR_SHIFTING : HUD_GEAR_ENGAGED);
	const float turboNeedle = turboNeedleAngle(f->turboBoost);

	// NitrousGauge::Update (0x8010D324), the path one human player outside a
	// drag race takes (TheRaceParameters +0x24 == 1, +0x40 and
	// g_tweakIsDragRace clear -- the port has neither split screen nor drag
	// races, whose bars are SetHorizontal/VerticalBarValue on NOS_Line).
	// Nothing left hides the line and the gamebreaker. Up to 1 the line is
	// hidden and the gamebreaker shows what is left through NOS_Mask; past 1
	// the line is up whole and the gamebreaker, in the overfill colour, shows
	// the overfill.
	const float nosPct = f->nos;
	const bool nosOver = nosPct > 1.0f;
	const float nosMaskDeg = HUD_NOS_MASK_FULL -
		(HUD_NOS_MASK_SWEEP - (nosOver ? nosPct - 1.0f : nosPct) * HUD_NOS_MASK_SWEEP);

	// The race widgets' text, LapCounter::Update (0x80109E10), Countdown::
	// Update (0x8010E8C8) and RaceOverMessage::Update (0x8010E1B4) in turn.
	const Race* r = f->race;
	splitTimeUpdate(h, f);
	char lapNumber[8] = "", numLaps[12] = "", curTime[16] = "", titleTime[16] = "";
	const char* lapText = h->w[HE_LAP_TEXT].text;
	const char* title = NULL;
	char cdText[16] = "";
	float cdScale = 1.0f;
	u32 cdColor = 0;
	int cdAlpha = 0;
	char roText[128] = "";
	float roMs = 0.0f;
	// RacePosition::Update's RaceTime: the race time, held 3 s (0x8032612C)
	// from each lap the car begins.
	if (r && f->racePos) {
		if (r->car.newLap)
			h->rpNewLapAt = r->now;
		if ((float)(r->now - h->rpNewLapAt) * 0.00025f >= 3.0f)
			h->rpTime = r->raceTime;
	}
	if (r) {
		const RacingCar* car = &r->car;
		// TheRaceParameters - 0x104E < 6: the six drift tracks 4174..4179
		// count percent, as a sprint does.
		const bool drift = (unsigned)(r->params.track - 0x104E) < 6u;
		const float done = car->lapExact > 0.0f ? car->lapExact : 0.0f;
		if (drift || r->params.pointToPoint) {
			// The sprint arm takes this layout on every build region but 2;
			// the drift arm always does.
			snprintf(numLaps, sizeof(numLaps), "%3d%%", (int)(done * 100.0f));
			lapText = h->sCompleted;
		} else {
			snprintf(numLaps, sizeof(numLaps), "/%2d", r->laps);
			snprintf(lapNumber, sizeof(lapNumber), "%2d", car->maxLap + 1);
		}

		const s32 now = r->now;
		const s32 last = race_last_start_crossing(r);
		if (tickSet(last)) {
			if (car->newLap) {
				h->lcNewLapAt = now;
				h->lcShown = race_last_lap_time(r);
				h->lcTitleAt = now;
				h->lcBest = !(race_best_lap_time(r) < h->lcShown || !h->lcHadLap);
				h->lcHadLap = true;
			}
			// The finished lap's time stays up for 3 s (0x8032648C), then
			// the new lap's runs.
			if ((float)(now - h->lcNewLapAt) * 0.00025f > 3.0f)
				h->lcShown = now - last;
			race_print_time(h->lcShown, 4, curTime);
			// And the title for 2 s (0x80326490).
			if ((float)(now - h->lcTitleAt) * 0.00025f < 2.0f) {
				snprintf(titleTime, sizeof(titleTime), "%s", curTime);
				title = h->lcBest ? h->sBestLapTime : h->sLapTime;
			}
		} else {
			snprintf(curTime, sizeof(curTime), "%s", h->sNoTime);
		}

		const int cd = race_countdown_number(r);
		if (cd != 0) {
			if (cd == 4)
				snprintf(cdText, sizeof(cdText), "%s", h->sGo);
			else
				snprintf(cdText, sizeof(cdText), "%d", cd);
			const float ratio = race_countdown_ratio(r);
			// bCos of ratio x 90 degrees in bAngle units (0x803269B8 = 90,
			// 0x803269BC = 65536, / 0x168), times 255 (0x803269C0).
			const int ang = ((int)(ratio * 90.0f * 65536.0f) / 0x168) & 0xFFFF;
			int a = (int)(cosf((float)ang * (2.0f * (float)M_PI / 65536.0f)) * 255.0f);
			a = a < 0 ? 0 : (a > 255 ? 255 : a);
			const HudWidget* w = &h->w[HE_COUNTDOWN];
			cdColor = C2D_Color32(w->rgba[0], w->rgba[1], w->rgba[2], (u8)a);
			cdAlpha = a;
			// 2 x ratio + 1 (0x803269D0).
			cdScale = ratio + ratio + 1.0f;
		}

		// The reasons the port's races end on, with one human racer
		// (TheRaceParameters +0x24, so no "%s\n%s %s" place line) in the
		// career (DAT_803CE800 1, not the 8 / 0x200 modes that add
		// 0xE863DD5C): reason 1 "%s %s", "Your time:" and
		// PrintToString(raceTime, 0); 8, the drag's blown engine, and 9, its
		// totaled car, a line of their own.
		// The LAN Outrun's two (#488), reasons 6 and 7 on the race's own car.
		const bool otr = car->finished && (car->finishReason == RACE_FINISH_OUTRUN_WON ||
			car->finishReason == RACE_FINISH_OUTRUN_LOST);
		const bool ro = car->finished && (car->finishReason == 1 || otr ||
			car->finishReason == RACE_FINISH_BLOWN || car->finishReason == RACE_FINISH_TOTALED);
		if (ro) {
			if (!h->roStarted) {
				h->roStarted = true;
				h->roStartedAt = now;
			}
			if (otr) {
				// Reason 6's "You Won the Outrun!" / 7's "You Lost the
				// Outrun!" and the time. PORT: no Bank Reward line -- a LAN
				// race pays no purse.
				char t[16];
				race_print_time(car->raceTime, 0, t);
				snprintf(roText, sizeof(roText), "%s\n%s %s",
					car->finishReason == RACE_FINISH_OUTRUN_WON ? h->sWonOutrun : h->sLostOutrun,
					h->sYourTime, t);
			} else if (car->finishReason == RACE_FINISH_BLOWN) {
				snprintf(roText, sizeof(roText), "%s", h->sBlown);
			} else if (car->finishReason == RACE_FINISH_TOTALED) {
				snprintf(roText, sizeof(roText), "%s", h->sTotaled);
			} else if (f->drift || drift) {
				// The drift arm: "%s %$d" of 0x13640DE9 and the banked points.
				char g[16];
				stuntGrouped(f->driftScore, g, sizeof(g));
				snprintf(roText, sizeof(roText), "%s %s", h->sYourScore, g);
			} else {
				char t[16];
				race_print_time(car->raceTime, 0, t);
				snprintf(roText, sizeof(roText), "%s %s", h->sYourTime, t);
			}
			roMs = (float)(now - h->roStartedAt) * 0.25f;
		}
	} else if (ga->overUp) {
		// The Outrun's, once EndGetAway has put the race type back to 0
		// (ShouldWeShowRaceOverMessage, 3 s): reason 6 "%s\n%s %s\n%s %$d" of
		// "You Won the Outrun!", "Your time:", the time, "Bank Reward:" and
		// the purse; reason 7 "%s\n%s %s" of "You Lost the Outrun!" and the
		// time. The script runs from the finish.
		char t[16], g[16];
		race_print_time((s32)(ga->raceSec * 4000.0f), 0, t);
		if (ga->won) {
			stuntGrouped(ga->purse, g, sizeof(g));
			snprintf(roText, sizeof(roText), "%s\n%s %s\n%s %s", h->sWonOutrun, h->sYourTime, t,
				h->sBankReward, g);
		} else {
			snprintf(roText, sizeof(roText), "%s\n%s %s", h->sLostOutrun, h->sYourTime, t);
		}
		roMs = ga->overSec * 1000.0f;
	}

	if (h->drag && f->drag) {
		dragDraw(h, f, nowMs, font, fontUnitPx, cdText, cdAlpha, cdScale, roText, roMs);
		return;
	}

	// A drift's own objects go in among the cluster's by depth, furthest first.
	int dk = 0;
	for (int k = 0; k < h->orderCount; k++) {
		const int e = h->order[k];
		while (dp && dk < dp->orderCount && dp->w[dp->order[dk]].depth > h->w[e].depth)
			driftDrawElem(h, dp, dp->order[dk++], f, nowMs, font, fontUnitPx);
		// Without a race only the race-over message may draw, the Outrun's.
		if (isRaceElem(e) && !r && !((e == HE_RACE_OVER || e == HE_RACE_OVER_BACKING) &&
				roText[0]))
			continue;
		if ((isGaugeElem(e) && !f->gauges) || (isLapElem(e) && !f->lapInfo) ||
				(isStuntElem(e) && !f->score) || (isNosElem(e) && !f->nosFitted) ||
				(isTurboElem(e) && !f->turbo))
			continue;
		// The art is authored grey and coloured by multiplying, as the
		// engine's vertex colour does. Text keeps citro2d's default mode: its
		// glyph sheets carry alpha only.
		C2D_SetTintMode(e < HE_IMAGES ? C2D_TintMult : C2D_TintSolid);
		switch (e) {
		case HE_NEEDLE:
			drawImage(h, e, needle);
			break;
		case HE_TURBO_DIAL:
			drawImage(h, e, turboNeedle);
			break;
		case HE_NOS_LINE:
			if (nosOver)
				drawImage(h, e, 0.0f);
			break;
		case HE_NOS_GB:
			// The art is a white-on-black mask, not a picture: the composite
			// takes the colour from FEngSetColor and the shape from the art.
			C2D_SetTintMode(C2D_TintSolid);
			if (nosPct > 0.0f)
				drawNosGameBreaker(h, nosMaskDeg, nosOver ? HUD_NOS_COLOUR_OVER : HUD_NOS_COLOUR);
			break;
		case HE_NOS_MASK:
			// Never drawn itself: it is what the gamebreaker is cut by.
			break;
		case HE_SHIFT:
			// #500, Tachometer::Update (0x8010953C): Shift_light is up while
			// the tach's revs are past GetOptimalShiftUpRPM(gear) less
			// PerfectShiftSplit (0.95) x GetPerfectShiftRange(gear) -- the
			// DriveTrain's PerfectShiftMaxRPMDiff[class][gear] x its scale.
			if (f->sim) {
				const int g = f->sim->dtGear;
				const float opt = carsim_optimal_shift_rpm_gear(f->sim, g);
				const float range = carsim_perfect_shift_range(f->sim, g);
				if (opt > 0.0f && f->rpm > opt - range * HUD_PERFECT_SHIFT_SPLIT) {
					drawImage(h, e, 0.0f);
					// Debug: the first time it lights in each gear.
					static int s_shiftLitGear = -1;
					if (g != s_shiftLitGear) {
						char b[128];
						s_shiftLitGear = g;
						snprintf(b, sizeof(b), "shift light on in gear %d at %.0f rpm (shift %.0f, range %.0f)",
						         g, (double)f->rpm, (double)opt, (double)range);
						dbglog_line("HUD ", b);
					}
				}
			}
			break;
		case HE_WRONG_WAY:
			drawWrongWay(h, nowMs);
			break;
		case HE_SPEED:
			drawString(h, e, speed, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_UNITS:
			drawString(h, e, h->unitText, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_GEAR:
			drawString(h, e, gear, gearColor, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_LAP_TEXT:
			drawString(h, e, lapText, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_LAP_NUMBER:
			drawString(h, e, lapNumber, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_NUM_LAPS:
			drawString(h, e, numLaps, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_TIME_TEXT:
			drawString(h, e, h->w[e].text, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_CUR_TIME:
			drawString(h, e, curTime, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_LAP_TITLE:
			if (title)
				drawString(h, e, title, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_LAP_TITLE_TIME:
			if (title)
				drawString(h, e, titleTime, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_COUNTDOWN:
			if (cdText[0])
				drawString(h, e, cdText, cdColor, font, fontUnitPx, cdScale, 0.0f);
			break;
		case HE_RACE_OVER:
			if (roText[0]) {
				const int a = (int)scriptTrack(kRoKeyMs, kRoMsgAlpha, 4, roMs);
				const float sc = scriptTrack(kRoKeyMs, kRoMsgScale, 4, roMs);
				drawString(h, e, roText, C2D_Color32(HUD_RO_MSG_GREY, HUD_RO_MSG_GREY,
					HUD_RO_MSG_GREY, (u8)a), font, fontUnitPx, sc, HUD_RO_MSG_Y - h->w[e].y);
			}
			break;
		case HE_RACE_OVER_BACKING:
			if (roText[0]) {
				const int a = (int)scriptTrack(kRoKeyMs, kRoBackAlpha, 4, roMs);
				const float bw = HUD_RO_BACK_W * HUD_SCALE, bh = HUD_RO_BACK_H * HUD_SCALE;
				C2D_DrawRectSolid(screenX(e, 0.0f) - bw * 0.5f, screenY(0.0f) - bh * 0.5f,
					0.5f, bw, bh, C2D_Color32(0, 0, 0, (u8)a));
			}
			break;
		case HE_GA_BACKING:
		case HE_GA_LINES:
			if (h->gaActive)
				drawImage(h, e, 0.0f);
			break;
		case HE_GA_BAR:
			if (h->gaActive)
				gaDrawBar(h, gaBar, gaBarArgb);
			break;
		case HE_GA_NUMBER:
			if (h->gaActive) {
				// "Ahead" / "Behind", 800 ms, looping: alpha 255 -> 200 ->
				// 255 and size 0.7 -> 0.5 -> 0.7 at 0 / 400 / 800 ms.
				const HudWidget* w = &h->w[e];
				const float ms = fmodf((float)(nowMs - h->gaPulseMs), HUD_GA_PULSE_MS);
				const int a = (int)scriptTrack(kGaPulseKeyMs, kGaPulseAlpha, 3, ms);
				const float sz = scriptTrack(kGaPulseKeyMs, kGaPulseSize, 3, ms);
				drawString(h, e, gaNumber, C2D_Color32(w->rgba[0], w->rgba[1], w->rgba[2], (u8)a),
					font, fontUnitPx, w->h > 0.0f ? sz / w->h : 1.0f, 0.0f);
			}
			break;
		case HE_GA_TEXT:
			if (h->gaActive)
				drawString(h, e, gaUnit, h->w[e].color, font, fontUnitPx, 1.0f, 0.0f);
			break;
		case HE_OUTRUN_MSG:
			if (h->orShown) {
				const float ms = (float)(nowMs - h->orStartMs);
				if (ms >= HUD_OR_MS) {
					h->orShown = false;
					break;
				}
				const HudWidget* w = &h->w[e];
				const int a = (int)scriptTrack(kCrKeyMs, kCrAlpha, 4, ms);
				const float sz = scriptTrack(kCrScaleKeyMs, kOrSize, 2, ms);
				drawString(h, e, h->orText, C2D_Color32(w->rgba[0], w->rgba[1], w->rgba[2], (u8)a),
					font, fontUnitPx, (w->h > 0.0f ? sz / w->h : 1.0f) * HUD_NOTICE(HUD_NOTICE_OUTRUN),
					HUD_OR_Y - w->y);
			}
			break;
		case HE_SPLIT_TEXT:
			// ZoomIn's script, then INIT's alpha 0 until Update hides it.
			if (r && h->stZoomed) {
				const float ms = (float)(r->now - h->stZoomAt) * 0.25f;
				if (ms < 0.0f || ms >= HUD_ST_MS)
					break;
				const HudWidget* w = &h->w[e];
				const int a = (int)scriptTrack(kStKeyMs, kStAlpha, 2, ms);
				const float sz = scriptTrack(kStKeyMs, kStSize, 2, ms);
				const u32 rgb = h->stGreen ? HUD_ST_GREEN : HUD_ST_RED;
				drawString(h, e, h->stText, C2D_Color32((u8)(rgb >> 16), (u8)(rgb >> 8), (u8)rgb,
					(u8)a), font, fontUnitPx, (w->h > 0.0f ? sz / w->h : 1.0f) *
					HUD_NOTICE(HUD_NOTICE_SPLIT), 0.0f);
			}
			break;
		case HE_SP_TYPE:
			stuntDrawStr(h, e, &h->spName, f->worldNow, font, fontUnitPx);
			break;
		case HE_SP_POINT:
			stuntDrawStr(h, e, &h->spPoints, f->worldNow, font, fontUnitPx);
			break;
		case HE_CASH_REWARD:
			if (h->crShown) {
				const float ms = (float)(nowMs - h->crStartMs);
				if (ms >= HUD_CR_MS) {
					h->crShown = false;
					break;
				}
				const HudWidget* w = &h->w[e];
				const int a = (int)scriptTrack(kCrKeyMs, kCrAlpha, 4, ms);
				const float sc = scriptTrack(kCrScaleKeyMs, kCrScale, 2, ms);
				drawString(h, e, h->crText, C2D_Color32(w->rgba[0], w->rgba[1], w->rgba[2],
					(u8)a), font, fontUnitPx, sc * HUD_NOTICE(HUD_NOTICE_CASH), HUD_CR_Y - w->y);
			}
			break;
		default:
			drawImage(h, e, 0.0f);
			break;
		}
	}
	while (dp && dk < dp->orderCount)
		driftDrawElem(h, dp, dp->order[dk++], f, nowMs, font, fontUnitPx);
	C2D_SetTintMode(C2D_TintSolid);
}

// "%$d": the amount with its thousands grouped and no currency sign, the way
// FEngPrintf prints the flag.
static void groupDigits(int v, char* out, size_t cap) {
	char d[16];
	const int n = snprintf(d, sizeof(d), "%d", v < 0 ? -v : v);
	size_t o = 0;
	if (v < 0 && o + 1 < cap)
		out[o++] = '-';
	for (int i = 0; i < n && o + 1 < cap; i++) {
		if (i > 0 && (n - i) % 3 == 0 && o + 2 < cap)
			out[o++] = ',';
		out[o++] = d[i];
	}
	out[o] = '\0';
}

void hud_show_showcase_reward(Hud* h, int magType, int baseReward, int starReward) {
	if (!h)
		return;
	// QueueMessage (0x80108B90): the type, four frames, the two amounts.
	// Update prints "%s%s: %$d" -- the label, the language's own space before
	// a colon (none in English) and the sum -- for a magazine (1, 3) or a DVD
	// cover (2, 4); any other type leaves the last text.
	char amt[24];
	groupDigits(baseReward + starReward, amt, sizeof(amt));
	if (magType == 1 || magType == 3)
		snprintf(h->crText, sizeof(h->crText), "%s: %s", h->sMagReward, amt);
	else if (magType == 2 || magType == 4)
		snprintf(h->crText, sizeof(h->crText), "%s: %s", h->sDvdReward, amt);
	h->crQueued = true;
	h->crDelay = 4;
	if (h->log) {
		char b[112];
		snprintf(b, sizeof(b), "hud: ShowShowcaseReward -- \"%s\" queued", h->crText);
		h->log(b);
	}
}

void hud_debug_outrun(Hud* h) {
	if (!h)
		return;
	gaMessage(h, h->sLeadGot, osGetTime());
	if (h->log) {
		const HudWidget* w = &h->w[HE_OUTRUN_MSG];
		const HudWidget* c = &h->w[HE_CASH_REWARD];
		char b[200];
		snprintf(b, sizeof(b), "hud: DbgHudNotice -- Out_Run_Message \"%s\" rgb %u,%u,%u "
			"justify 0x%lx; Cash_Reward rgb %u,%u,%u justify 0x%lx", h->orText, w->rgba[0],
			w->rgba[1], w->rgba[2], (unsigned long)w->justify, c->rgba[0], c->rgba[1],
			c->rgba[2], (unsigned long)c->justify);
		h->log(b);
	}
}

// ---- the race stats on the bottom screen (TASKS.md #268 D) ------------------
// RacePosition and LeaderBoard (hud.h). Retail draws them in the canvas's
// top-right corner over the street; the port moves them beside the race's
// minimap on the bottom screen (CONTINUE.MD 1a #17). The block keeps its .fng
// layout, mapped whole into the column: PositionBacking's left edge to
// LBBacking's right fills the column's width, the backing's top at its top.
// Retail's backing runs off the canvas's right edge and the column's runs off
// the screen's the same way.

#define HUD_ST_MARGIN 4.0f
// LeaderBoard::Update's two holds (0x80326598 / 0x80326594), in seconds.
#define HUD_LB_TIMES_SEC 10.0f
#define HUD_LB_NAMES_SEC 2.0f
// The rows' "SetPlayerColor" scripts: LBData to (202,244,138), LBBacking to
// opaque black. "Init" puts back the colours the .fng authors.
#define HUD_LB_PLAYER_TEXT C2D_Color32(202, 244, 138, 255)
#define HUD_LB_PLAYER_BACK C2D_Color32(0, 0, 0, 255)

// TimeAroundTrackTable (ctor 0x80225DDC, GetTimeAroundTrack 0x80226568): the
// seconds `lap` laps round the route take, never fewer than none. Retail
// builds it from the TrackRoutePoints' posted speeds; with no route points
// every entry is the default 40 m/s (0x8033AD7C) over the route's length.
// PORT: the posted speeds are not packed (race.c, extrapolateRaceTime), so
// the table is always that fallback.
#define HUD_TAT_SPEED 40.0f
static float timeAroundTrack(float lap, float routeLen) {
	return (lap > 0.0f ? lap : 0.0f) * routeLen / HUD_TAT_SPEED;
}

// Timer::SetTime(float), seconds to ticks.
static s32 secTicks(float sec) { return (s32)(sec * 4000.0f); }

// CalculateSplitTime (0x8022A0C0): how far `car` runs behind the player `pl`,
// in ticks -- negative for a car ahead. A finish is reason 1.
// PORT: gameMode 7's arm, a distance difference, is left out: every port race
// is mode 1, and DeterminRaceInfo gives mode 7 no leaderboard.
static s32 calcSplitTime(const Race* r, const RacingCar* car, const RacingCar* pl, float len) {
	if (car->finishReason == 1) {
		if (pl->finishReason == 1)
			return pl->raceTime - car->raceTime;
		return secTicks(timeAroundTrack(pl->lapExact, len) - timeAroundTrack(car->maxLap, len)) -
			((r->now - car->raceTime) - r->timerBase);
	}
	if (pl->finishReason == 1)
		return secTicks(timeAroundTrack(pl->maxLap, len) - timeAroundTrack(car->lapExact, len)) +
			((r->now - pl->raceTime) - r->timerBase);
	return secTicks(timeAroundTrack(pl->lapExact, len) - timeAroundTrack(car->lapExact, len));
}

// The car in race place `place` (1 = leading), or NULL.
static const RacingCar* carInPlace(const Race* r, int place) {
	const int n = race_car_count(r);
	for (int i = 0; i < n; i++) {
		if (race_position(r, i) == place)
			return race_car(r, i);
	}
	return NULL;
}

// SplitTime::ZoomIn (0x8010AB78): once per showing, ZoominGreen for a time at
// or under 0, else ZoominRed, and the string visible.
static void splitTimeZoomIn(Hud* h, const Race* r) {
	if (h->stZoomed)
		return;
	h->stZoomAt = r->now;
	h->stGreen = h->stSplit <= 0;
	h->stZoomed = true;
	if (h->log) {
		char b[96];
		snprintf(b, sizeof(b), "hud: SplitTime \"%s\" (%s)", h->stText,
			h->stGreen ? "ZoominGreen" : "ZoominRed");
		h->log(b);
	}
}

// SplitTime::Update (Speed.elf 0x8010A6F4, SPEED2.EXE 0x4DA090) on the
// player's RacingCar, with ShowRaceLeader / ShowLapRecord / ShowBestLap /
// ShowLastLap (0x8010A870 / 0x8010AA70 / 0x8010A978 / 0x8010AAE4). Nothing
// new this frame: 3 s after ZoomIn the string goes. A new boundary or lap:
// the row's time, printed with a sign (PrintToString flags 2), and ZoomIn.
static void splitTimeUpdate(Hud* h, const HudFrame* f) {
	const Race* r = f->race;
	// Feature 0x80 (SPEED2.EXE 0x5F33DD): a race, outside a drift, with the
	// row on. The drag's own package has no SplitTimeText.
	if (!r || f->drift || f->drag || f->splitTime > 3) {
		h->stZoomed = false;
		return;
	}
	const RacingCar* car = &r->car;
	h->stSplit = 0;
	if (!car->newLap && !car->newProgress) {
		if ((float)(r->now - h->stZoomAt) * 0.00025f > HUD_ST_HIDE_SEC)
			h->stZoomed = false;
		return;
	}
	const s32 last = car->split[r->finishIdx];   // GetLastLapTime
	const int maxLap = car->maxLap;
	switch (f->splitTime) {
	case 0: {
		// Against the leader, or the second car when the player leads; and
		// only a time that is not 0.
		if (race_car_count(r) < 2)
			return;
		const RacingCar* other = carInPlace(r, race_position(r, 0) == 1 ? 2 : 1);
		if (!other)
			return;
		const float len = r->route ? race_route_length(r->route) : 0.0f;
		h->stSplit = calcSplitTime(r, car, other, len);
		if (h->stSplit == 0)
			return;
		break;
	}
	case 1:
		if (!car->newLap)
			return;
		h->stSplit = last;
		break;
	case 2:
		// Against the race's best lap; a lap that set it is measured by the
		// lap before it instead.
		if (!car->newLap)
			return;
		h->stSplit = last - r->bestLap;
		if (h->stSplit == 0 && maxLap >= 1)
			h->stSplit = car->lapTime[maxLap - 1] - r->bestLap;
		break;
	default:
		// The last lap against the one before it, from the second lap on.
		if (!car->newLap || maxLap <= 1 || maxLap >= RACE_MAX_LAPS)
			return;
		h->stSplit = car->lapTime[maxLap] - car->lapTime[maxLap - 1];
		break;
	}
	race_print_time(h->stSplit, 2, h->stText);
	splitTimeZoomIn(h, r);
}

// GetLocalizedOrdinal (0x80121394): 10 has its own key, the rest go by their
// last digit (OrdinalHashes: th st nd rd, then th).
static const char* localizedOrdinal(const Hud* h, int n) {
	if (n == 10)
		return h->sOrd10;
	const int d = n % 10;
	return h->sOrd[d < 4 ? d : 0];
}

// SortLists (0x8010BD94): which race places the four rows show, as slots
// (place - 1). Row 0 the leader; then the cars just ahead of the player, as
// many as leave a row for it, nearest last; the player unless it leads; then
// the cars behind it until the rows are full. Rows it leaves are 0 (Update's
// bMemSet), the leader.
static void lbSortRows(int playerPos, int count, int* rows) {
	int ahead[HUD_LB_ROWS], behind[HUD_LB_ROWS], nA = 0, nB = 0;
	int left = HUD_LB_ROWS - 1, a = playerPos - 2, b = playerPos;
	for (int k = 0; k < count && left > 0; k++) {
		if (a > 0 && left - 1 > 0) {
			ahead[nA++] = a--;
			left--;
		}
		if (b < count && left > 0) {
			behind[nB++] = b++;
			left--;
		}
	}
	for (int i = 0; i < HUD_LB_ROWS; i++)
		rows[i] = 0;
	int n = 1;
	while (nA > 0)
		rows[n++] = ahead[--nA];
	if (playerPos - 1 != 0)
		rows[n++] = playerPos - 1;
	for (int k = 0; k < nB && n < HUD_LB_ROWS; k++)
		rows[n++] = behind[k];
}

// Canvas to screen for the column: screen = origin + canvas * px.
typedef struct {
	float x0, y0, px;
} HudStatMap;

static void statRect(const HudStatMap* m, const HudWidget* w, u32 color) {
	C2D_DrawRectSolid(m->x0 + (w->x - w->w * 0.5f) * m->px, m->y0 + (w->y - w->h * 0.5f) * m->px,
		0.5f, w->w * m->px, w->h * m->px, color);
}

static void statImage(const HudStatMap* m, const HudWidget* w, C3D_Tex* tex,
	const Tex3DS_SubTexture* sub)
{
	C2D_Image img = { tex, sub };
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, w->color, 1.0f);
	C2D_SetTintMode(C2D_TintMult);
	C2D_DrawImageAt(img, m->x0 + (w->x - w->w * 0.5f) * m->px,
		m->y0 + (w->y - w->h * 0.5f) * m->px, 0.5f, &tint,
		w->w * m->px / (float)sub->width, w->h * m->px / (float)sub->height);
	C2D_SetTintMode(C2D_TintSolid);
}

static void statText(Hud* h, const HudStatMap* m, const HudWidget* w, const char* str,
	u32 color, C2D_Font font, float fontUnitPx)
{
	if (w->found)
		drawTextIn(h->stBuf, w, m->x0, m->y0, m->px, str, color, font, fontUnitPx, 1.0f,
			0.0f, 0.0f);
}

// RacePosition::Update's three strings: the player's place, its ordinal and
// "/" the race's car count -- in a drift, the place on DriftManager's
// leaderboard and its length.
static void statPosition(Hud* h, const HudStatMap* m, const HudWidget* number,
	const HudWidget* nth, const HudWidget* racers, int pos, int count, C2D_Font font,
	float fontUnitPx)
{
	char t[16];
	snprintf(t, sizeof(t), "%d", pos);
	statText(h, m, number, t, number->color, font, fontUnitPx);
	statText(h, m, nth, localizedOrdinal(h, pos), nth->color, font, fontUnitPx);
	snprintf(t, sizeof(t), "/%d", count);
	statText(h, m, racers, t, racers->color, font, fontUnitPx);
}

void hud_draw_race_stats(Hud* h, const HudRaceStats* s, C2D_Font font, float fontUnitPx,
	float x, float y, float w)
{
	if (!h || !s->race || !h->stBuf)
		return;
	C2D_TextBufClear(h->stBuf);
	const Race* r = s->race;
	int count = race_car_count(r);
	if (count > 1 + RACE_MAX_AI)
		count = 1 + RACE_MAX_AI;
	int playerPos = race_position(r, 0), posCount = count;
	if (s->drift) {
		playerPos = s->driftPos;
		posCount = s->driftCount;
	}
	// DeterminRaceInfo (0x80216344): RacePosition's 0x200 with the Position
	// row on unless the race is one car; LeaderBoard's 0x800000 with the
	// Leaderboard row on outside a drag (and online play, mode 7 and free
	// run, which the port does not have).
	const bool showPos = s->racePos && posCount > 1 && playerPos > 0;
	const bool showLb = s->leaderboard && !s->drag && count > 0 && playerPos > 0;

	// The column's scale: the single race's block across its width.
	const HudWidget* back = &h->st[SE_BACKING];
	const HudWidget* lbBack = &h->st[SE_LB_BACK];
	const float left = back->found ? back->x - back->w * 0.5f : 129.0f;
	const float right = lbBack->found ? lbBack->x + lbBack->w * 0.5f : 287.5f;
	HudStatMap m;
	m.px = (w - 2.0f * HUD_ST_MARGIN) / (right - left);
	m.x0 = x + HUD_ST_MARGIN - left * m.px;
	m.y0 = y + HUD_ST_MARGIN - (back->found ? back->y - back->h * 0.5f : -214.0f) * m.px;

	if (showPos && s->drag && h->drag) {
		// HUD_Drag.fng's: Drag_Position_Backing_01 and the three strings,
		// its own block put where the single race's goes.
		const struct HudDrag* d = h->drag;
		const HudWidget* db = &d->w[DE_POS_BACKING];
		HudStatMap dm = m;
		if (db->found) {
			dm.x0 = x + HUD_ST_MARGIN - (db->x - db->w * 0.5f) * m.px;
			dm.y0 = y + HUD_ST_MARGIN - (db->y - db->h * 0.5f) * m.px;
			if (d->texValid[DE_POS_BACKING])
				statImage(&dm, db, (C3D_Tex*)&d->tex[DE_POS_BACKING], &d->sub[DE_POS_BACKING]);
		}
		statPosition(h, &dm, &d->w[DE_POS_NUMBER], &d->w[DE_POS_NTH], &d->w[DE_POS_RACERS],
			playerPos, posCount, font, fontUnitPx);
	} else if (showPos) {
		if (h->stTexValid && back->found)
			statImage(&m, back, &h->stTex, &h->stSub);
		statPosition(h, &m, &h->st[SE_NUMBER], &h->st[SE_NTH], &h->st[SE_RACERS], playerPos,
			posCount, font, fontUnitPx);
	}
	if (!showLb)
		return;

	// Update (0x8010AEA4), one human outside mode 7: the gaps (a drift's
	// scores) for 10 s, the names for 2 s, in turn; the switch shows from the
	// next frame.
	if (h->lbSince == 0)
		h->lbSince = r->now;
	const bool names = h->lbNames;
	const float since = (float)(r->now - h->lbSince) * 0.00025f;
	if (since > (names ? HUD_LB_NAMES_SEC : HUD_LB_TIMES_SEC)) {
		h->lbSince = r->now;
		h->lbNames = !names;
	}

	if (s->drift) {
		// The drift arm (UpdateDriftScore 0x8010B858): DriftManager's board
		// from the top, the most points first -- no race order, no gaps.
		// The player's row gets LBBacking's SetPlayerColor; SetSlotColour is
		// empty there, so the strings keep their authored colours. Rows the
		// board has no car for hide their two strings, not their backings.
		for (int i = 0; i < HUD_LB_ROWS; i++) {
			const bool on = i < s->driftRows;
			const bool me = on && s->driftMe[i];
			const HudWidget* tb = &h->st[SE_LB_TEXT_BACK + i];
			const HudWidget* bb = &h->st[SE_LB_BACK + i];
			if (tb->found)
				statRect(&m, tb, tb->color);
			if (bb->found)
				statRect(&m, bb, me ? HUD_LB_PLAYER_BACK : bb->color);
			if (!on)
				continue;
			const HudWidget* data = &h->st[SE_LB_DATA + i];
			char t[24];
			if (names) {
				// DrawDriftPlayerNames (0x8010BA7C): "%s".
				const int id = s->driftId[i];
				const char* name = s->driftName[i] ? s->driftName[i] :
					id >= 0 && id <= RACE_MAX_AI ? s->names[id] : NULL;
				statText(h, &m, data, name ? name : "", data->color, font, fontUnitPx);
			} else {
				// DrawDriftScores (0x8010B8F4): "%$d" of the score.
				stuntGrouped((int)s->driftPts[i], t, sizeof(t));
				statText(h, &m, data, t, data->color, font, fontUnitPx);
			}
			const HudWidget* lt = &h->st[SE_LEADER_TEXT + i];
			snprintf(t, sizeof(t), "%d :", i + 1);
			statText(h, &m, lt, t, lt->color, font, fontUnitPx);
		}
		return;
	}

	// SetupLists (0x8010BBBC): the racers in race order (Race +0xBD0), each
	// slot's place, name and gap -- none for a car that is home.
	int order[1 + RACE_MAX_AI];
	int place[1 + RACE_MAX_AI];
	for (int i = 0; i < count; i++) {
		place[i] = race_position(r, i);
		int j = i;
		while (j > 0 && place[order[j - 1]] > place[i]) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = i;
	}
	const float len = r->route ? race_route_length(r->route) : 0.0f;
	s32 split[1 + RACE_MAX_AI];
	for (int i = 0; i < count; i++) {
		const RacingCar* c = race_car(r, order[i]);
		split[i] = c && !c->finished ? calcSplitTime(r, c, &r->car, len) : 0;
	}
	int rows[HUD_LB_ROWS];
	lbSortRows(playerPos, count, rows);

	// The backings stay up on every row; the strings only on rows the race
	// has cars for.
	for (int i = 0; i < HUD_LB_ROWS; i++) {
		const int slot = rows[i];
		const bool me = i < count && slot + 1 == playerPos;
		const HudWidget* tb = &h->st[SE_LB_TEXT_BACK + i];
		const HudWidget* bb = &h->st[SE_LB_BACK + i];
		if (tb->found)
			statRect(&m, tb, tb->color);
		if (bb->found)
			statRect(&m, bb, me ? HUD_LB_PLAYER_BACK : bb->color);
		if (i >= count)
			continue;
		const HudWidget* data = &h->st[SE_LB_DATA + i];
		const u32 dataColor = me ? HUD_LB_PLAYER_TEXT : data->color;
		const RacingCar* c = race_car(r, order[slot]);
		const char* name = s->names[order[slot]];
		char t[24];
		if (names || me) {
			// DrawPlayerNames (0x8010B3CC), and DrawTimes' own row.
			statText(h, &m, data, name ? name : "", dataColor, font, fontUnitPx);
		} else if (c && !c->finished) {
			// DrawTimes (0x8010B528): PrintToString(split, 10), '+' and the
			// last digit kept.
			race_print_time(split[slot], 10, t);
			statText(h, &m, data, t, dataColor, font, fontUnitPx);
		} else if (c) {
			const char* why;
			switch (c->finishReason) {
			case 9: why = h->sTotaledLb; break;
			case 3: why = h->sKnockedOut; break;
			case 8: why = h->sBlownEngine; break;
			case 0xB: why = h->sTimeup; break;
			case 0xC: why = h->sDisconnected; break;
			default: why = h->sFinished; break;
			}
			statText(h, &m, data, why, dataColor, font, fontUnitPx);
		}
		const HudWidget* lt = &h->st[SE_LEADER_TEXT + i];
		snprintf(t, sizeof(t), "%d :", slot + 1);
		statText(h, &m, lt, t, lt->color, font, fontUnitPx);
	}
}
