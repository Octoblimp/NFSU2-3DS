#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#include "carsim.h"
#include "langbin.h"
#include "race.h"
#include "style.h"

// The in-game tachometer / speedometer / nitrous cluster (TASKS.md #146).
//
// This is retail's third-person tach, not a port-drawn gauge: the widgets,
// their positions, sizes, colours and draw order are HUD_SingleRace.fng's
// (InGameFreeRoam.bun), the art is the car's own HUD style out of
// HUD.nfsh (tools/hud_pack.py), and every runtime rule below is ported from
// the GameCube build's symbols (Speed.elf):
//
//   Tachometer ctor   0x80108D1C  widget lookup, HUD style, tints, sweep
//   Tachometer Update 0x8010953C  speed, units, gear letter + colour, needle
//   CalcAngleForRPM   0x801094B0  needle angle
//   GetTachStyle      0x80241F3C  which plate and redline band
//   NitrousGauge      0x8010D210 / 0x8010D324
//   TurboMeter        0x801098B0 / 0x801099B4
//   CunningStuntSpread 0x8010F1E0..0x801105CC  the style moment's name and
//                     points over the cluster (UpdateStylePoints, UpdateStunts,
//                     DrawStunt; TASKS.md #311)
//
// A drag race swaps in HUD_Drag.fng's package instead (hud_drag_open, TASKS.md
// #268 D): DragTachometer 0x8010C080 / 0x8010C458 (the tall tach down the
// left edge, its needle, redline band, shift light and shift messages),
// EngineTempGauge 0x8010D0A4 / 0x8010D13C and NitrousGauge's drag bars.
//
// The turbo gauge (TurboMeter) is up only for a car whose EngineInfo has a
// turbo value above 0, its dial turned by carsim's ComputeTurboPressure; the
// nitrous arc only with a bottle fitted (DetermineGauges' 0x40000).
//
// What is not drawn yet, and why (none of it is a guess left in):
//   * NitrousGauge's split-screen and race-type variants (vertical bars and
//     the needle) -- only the single-screen horizontal bars are drawn;
//   * 3rdperson_Turbolight, which no code touches and whose texture ships in
//     no container.
//
// Placement is the user's (CONTINUE.MD 1a #15): bottom-right of the top
// screen, as in the original game. The 640x480 canvas maps to the 400x240 top
// screen at half scale with the canvas's right and bottom edges on the
// screen's, which keeps the cluster exactly where the .fng puts it relative to
// that corner.

typedef struct Hud Hud;

// What the car's HUD parts say: RideInfo slots CarSlotId 0xA4..0xA7, i.e. a
// PresetCar's parts[0xA4..0xA7]. `stylePart` is the slot-0xA4 part (its
// HUDINDEX picks the art set), 0 when empty. `tint` is the RED/GREEN/BLUE of
// slots 0xA5 (fill), 0xA6 (needle) and 0xA7 (tick plate) as 0x00RRGGBB, or
// 0xFFFFFFFF for an empty slot, which the constructor draws white.
typedef struct {
	u32 stylePart;
	u32 tint[3];
} HudParts;

// The car numbers the cluster reads, all from the car record the simulation
// already uses: EngineInfo +0/+4/+8 is cardb's idle/redline/limiter.
typedef struct {
	float idleRpm, redlineRpm, limiterRpm;
} HudEngine;

// Builds the cluster. `nfshPath` is GLOBAL/HUD.nfsh; `fng`/`fngLen` the
// decompressed HUD_SingleRace.fng; `unitText` the translated speed-unit string
// (English.bin 0x8569AB44 "MPH" or 0x8569A25F "KPH"). NULL when the file or the
// screen is missing, which the caller treats as "no HUD" -- the game still
// runs. `lang` is English.bin for the race widgets' strings (GO!, LAP TIME,
// Your time: ...), NULL for the built-in English copies. Logs through `log`.
Hud* hud_open(const char* nfshPath, const u8* fng, u32 fngLen,
	const HudParts* parts, const HudEngine* eng, bool kph, const char* unitText,
	const LangTable* lang, void (*log)(const char*));

void hud_close(Hud* h);

// The rear-view mirror's art (#501), out of HUD.nfsh: RVM, the dark glass
// (black, alpha ~136 inside the trapezoid), and RVM_MASK, the trapezoid's
// cut-out. Both 128x64 RGBA4 with t = 1 at the top. False when the pack
// predates them.
bool hud_rvm_art(const Hud* h, const C3D_Tex** frame, const C3D_Tex** mask);

// The speed readout's units and their word, as hud_open() took them. PORT
// (TASKS.md #386): the world calls this each frame, so a change on the pause
// menu's Options shows at once rather than at the next open (the garage, or the
// main menu). NULL `unitText` gives the built-in "KPH"/"MPH".
void hud_set_units(Hud* h, bool kph, const char* unitText);

// The three tints again, as HudParts.tint (fill, needle, tick plate), for the
// specialties shop's Custom Gauges preview: HUD_Customizer's
// SetColorAttribute (0x8017BDA0) recolours its images as the swatch moves.
void hud_set_tints(Hud* h, const u32 tint[3]);

// The cluster's images (HudFrame.gauges) as hud_draw() puts them on the top
// screen: the box around them, in top-screen pixels. False with none loaded.
// PORT: for the Custom Gauges preview, drawn bigger on the bottom screen
// through a citro2d view transform.
bool hud_gauge_rect(const Hud* h, float* x0, float* y0, float* x1, float* y1);

// FEngHud's drag-race package over the open cluster: HUD_Drag.fng's widgets
// (InGameDrag.bun, decompressed in `fng`), their art out of the same
// HUD.nfsh, the plate and redline band by `eng`'s tach style, and the
// needle's one-player sweep, CarHudInfo +0xC / +0x10 (cardb's hudAngle[2] /
// [3]). False, having logged why, when the screen or the pack is missing:
// the race then keeps the cluster. Call as a drag starts; frames with
// HudFrame.drag set draw it.
bool hud_drag_open(Hud* h, const char* nfshPath, const u8* fng, u32 fngLen,
	const HudEngine* eng, float angStart, float angEnd, const LangTable* lang);

// Drops the drag package (none open is fine). Call once the drag is over,
// outside the frame.
void hud_drag_close(Hud* h);

// FEngHud's drift-race package over the open cluster (TASKS.md #268 E):
// HUD_Drift.fng (InGameDrift.bun, decompressed in `fng`) and its art out of
// the same HUD.nfsh.
//   DriftGauge ctor 0x8010D6CC, Update 0x8010DABC   the angle meter, Too
//                Slow!, the wrong-way countdown, the King drift names
//   CunningStuntSpread's drift arm (UpdateStylePoints 0x8010FFD0,
//                UpdateDriftStunts 0x8010F780, UpdateHypoMultiplier
//                0x8010F670)   the drift's points, the best drift, the
//                multiplier and what the next one needs, the total
// The cluster's own widgets (the tach, the lap box, the countdown, the
// race-over message) move to HUD_Drift.fng's places for the race; the ones
// the screen does not carry (the nitrous gauge -- DetermineGauges gives a
// drift none -- the lap title and the stunt strings) are not drawn.
// `turbo` is the DriftGauge constructor's test, the engine's boost curve
// (CarPhysicsInfo +0x224, nine values) above 0 somewhere: the angle
// meter's backing and its strings' place go by it. False, having logged
// why, when the screen or the pack is missing: the race keeps the cluster.
bool hud_drift_open(Hud* h, const char* nfshPath, const u8* fng, u32 fngLen, bool turbo,
	const LangTable* lang);

// Drops the drift package and puts the cluster's widgets back (none open is
// fine). Call once the drift is over, outside the frame.
void hud_drift_close(Hud* h);

// What the drift package reads each frame (HudFrame.dr): DriftManager's
// fields (drift.h's DriftView), the accumulator's DRIFT moment and the car.
typedef struct {
	int   level;        // DriftManager [10], the multiplier, 1..5
	int   next;         // [0xB], the points the next level needs; 0 at the top
	bool  losing;       // [7] -> SlowMessage's LosingMult
	bool  tooSlow;      // [8] -> TooSlowToGain
	float bonus;        // [9], the bonus zone's multiplier
	float dodge;        // [0x12], the traffic dodged
	float wrongWaySec;  // WrongWay_CountDown's seconds; shown above 0
	// CarState::GetYaw (carsim_yaw) in degrees, DriftGauge::Update's angle.
	float yawDeg;
	// StyleMoment DRIFT: its state (STYLE_STATE_*, -1 when not made), the
	// running drift's points (+0x24), the banked total (+0x2C) and the best
	// drift (+0x44).
	int   state;
	float points, banked, best;
	// ShowNiftyDriftyMessage this frame: the King level's name, else NULL.
	const char* nifty;
	// RacePosition's drift arm (#464): the Hurry! timer's seconds left
	// (race_hurry_left), below 0 while it has not started.
	float hurryLeft;
} HudDriftFrame;

// The Outrun's share of a frame (#268 H step 2, HudFrame.ga).
//   GetAwayMeter (ctor 0x801123E8, Update 0x8011252C, Show 0x801128EC, Hide
//   0x80112A98), HUD feature 0x40 while the race type is 7: the bar |d| / D
//   (SetHorizontalBarValue(v, 1, 99, 1): the art from pixel 99 to its full
//   width), green ahead, red behind, the lead in metres or feet over it, and
//   Out_Run_Message's ZoomIn -- "Outrun Challenge Accepted" on the third
//   Show, "You have the Lead!" / "You Lost the Lead!" as the lead changes
//   in race stage 4. The group goes Active on the fourth Show.
//   RaceOverMessage (0x8010E1B4) reasons 6 and 7 once EndGetAway has put the
//   race type back to 0, for ShouldWeShowRaceOverMessage's 3 s.
// The meter is anchored on the canvas's top-left, where retail puts it and
// where the Engage widget sits outside a race (DetermineEventInfo gives that
// one no bit while the race type is not 0).
typedef struct {
	bool  on;          // race type 7: the meter shows
	bool  racing;      // the race in stage 4
	float gap;         // GetDistanceFromLeader: <= 0 ahead by that much, > 0 behind
	float dist;        // GetGetAwayDistance
	bool  playerFirst; // the player's RacingCar is the leader (Show's message)
	bool  metric;      // DAT_803BC268: metres, else feet
	// RaceOverMessage after EndGetAway.
	bool  overUp;
	float overSec;     // since the player's finish
	bool  won;         // reason 6, else 7
	float raceSec;     // RacingCar +0x198
	int   purse;
} HudGetaway;

// One frame's inputs. `speedMs` is the car's speed in m/s (CarState+0x400),
// `gear` the port's (-1 reverse, 0 neutral, 1..n), `clutchOut` true while a
// shift has the clutch disengaged.
typedef struct {
	float rpm;
	float speedMs;
	int gear;
	bool clutchOut;
	// The race on, or NULL in free roam. With one, the race widgets draw:
	// LapCounter (laps or percent, the lap clock, the lap-time title),
	// Countdown and RaceOverMessage, all HUD_SingleRace.fng's own. The lap
	// box sits top-right, where the canvas puts it; the countdown, the lap
	// title and the race-over message are authored on the canvas's centre
	// line and go on the top screen's.
	const Race* race;
	// Player +0x234, DetectWrongWay's answer (race.h). WrongWIndi fades
	// WrongWayImage in while it is set and out when it clears.
	bool wrongWay;
	// The Player page's two HUD rows that switch an element this file draws.
	// Each HudElement shows while the feature mask has one of its own bits
	// (Toggle 0x80107E98): the Tachometer's are 0x10000060, LapCounter's
	// 0x508. DetermineGauges (0x802160E8) adds no bit at all while
	// PlayerSettings Gauges is off; DeterminRaceInfo (0x80216344) adds 0x508
	// only while Lap Info is on. So `gauges` hides the whole cluster (the
	// speed, gear and nitrous readouts are the Tachometer's own widgets) and
	// `lapInfo` the lap box and its titles. The countdown, race-over message
	// and wrong-way sign are other elements and stay.
	bool gauges;
	bool lapInfo;
	// CarState::GetNosPercentageLeft (carsim_nos_fraction): 0 for an empty
	// or missing bottle, 1 full, above 1 the GameBreaker overfill.
	float nos;
	// The style moments (CunningStuntSpread). `score` is the Player page's
	// Score row: DetermineScoring (0x80216310) adds the element's 0x20000 only
	// with it on, and FEUpdateStylePoints feeds it only then. `nosFitted` is
	// a bottle in the car (CarState +0x424 above 0): the constructor shows a
	// moment's points beside its name only then (+0x7C). `stunts` is what
	// StylePointsAccumulator::Update listed since the last frame
	// (style_take_up); `worldNow` is WorldTimer, in ticks of 1/4000 s, which
	// times how long a moment stays up and its scripts.
	bool score;
	// Also NitrousGauge's feature bit: DetermineGauges adds it only with a
	// bottle in the car, so without one the N2O arc is not drawn at all.
	bool nosFitted;
	// TurboMeter::Update: Car::GetTurboBoost's flag (EngineInfo has a turbo
	// value above 0) shows or hides the six turbo widgets, and its boost
	// (Engine +0x9C, carsim_turbo_boost) turns the dial.
	bool turbo;
	float turboBoost;
	const StyleUp* stunts;
	int stuntCount;
	s32 worldNow;
	// A drag race is on and hud_drag_open() gave it its package: the frame
	// draws that instead of the cluster. `sim` is the player's car, which the
	// DragTachometer asks for its shift bonus and window, and the
	// EngineTempGauge for its heat; NULL leaves both still.
	bool drag;
	const CarSim* sim;
	// RacePosition's feature bit (0x200), DeterminRaceInfo's rule: the
	// Player page's Position row on and a race of more than one car. On the
	// top screen it is only the drag's RaceTime and Drag_Position_Backing_02;
	// the place itself is hud_draw_race_stats()'s, on the bottom screen.
	bool racePos;
	// A drift (TheRaceParameters +0x44): RaceOverMessage::Update's reason-1
	// line is "%s %$d" of 0x13640DE9 and the accumulator's banked DRIFT
	// points, `driftScore` -- as it is on the downhill drift tracks.
	bool drift;
	int driftScore;
	// The Player page's Split Time row (PlayerSettings +0x14): 0 Leader,
	// 1 Lap Record, 2 Best Lap, 3 Last Lap, 4 Off. SplitTime (#502) shows
	// while the feature mask has its 0x80 -- SPEED2.EXE 0x5F33DD sets it for a
	// row other than Off, offline, outside a drift -- and Update's switch
	// (0x4DA0F1) picks what the time is measured against.
	u8 splitTime;
	// With `drift` and hud_drift_open()'s package: its inputs.
	HudDriftFrame dr;
	// The Outrun, free roam's race (see HudGetaway).
	HudGetaway ga;
} HudFrame;

// LapCounter's and RaceOverMessage's per-race state back to their
// constructors' values. Call when a race starts.
void hud_race_reset(Hud* h);

// Draws onto the current citro2d target (the top screen). `font` and
// `fontUnitPx` are the Conduit face the menus use and its measured line
// height.
void hud_draw(Hud* h, const HudFrame* f, C2D_Font font, float fontUnitPx);

// The race stats, HUD_SingleRace.fng's RacePosition and LeaderBoard (a drag:
// HUD_Drag.fng's RacePosition), moved to the bottom screen (TASKS.md #268 D,
// CONTINUE.MD 1a #17):
//
//   RacePosition ctor 0x80108190, Update 0x801083F8   the place, "nd", "/4"
//   LeaderBoard  ctor 0x8010AC90, Update 0x8010AEA4   four rows: the leader,
//                the cars round the player, their gaps and names in turn
//   SetupLists 0x8010BBBC, SortLists 0x8010BD94, DrawTimes 0x8010B528,
//   DrawPlayerNames 0x8010B3CC, CalculateSplitTime 0x8022A0C0,
//   GetLocalizedOrdinal 0x80121394
//
// DeterminRaceInfo (0x80216344) decides which show: the place with the
// Position row on in a race of more than one car, the leaderboard with the
// Leaderboard row on outside a drag.
typedef struct {
	const Race* race;
	// Each race_car() index's name: the profile's for the player (0), the
	// driver's for the rest (SetupLists). NULL prints nothing.
	const char* names[1 + RACE_MAX_AI];
	bool racePos;      // OptPlayer racePos
	bool leaderboard;  // OptPlayer leaderboard
	bool drag;         // a drag, with hud_drag_open()'s package
	// RacePosition::Update's drift arm (TheRaceParameters +0x44): the place
	// is GetLeaderBoardPos of the HUD car and the count the leaderboard's
	// length.
	bool drift;
	int driftPos, driftCount;
	// LeaderBoard's drift arm (UpdateDriftScore 0x8010B858, DrawDriftScores
	// 0x8010B8F4, DrawDriftPlayerNames 0x8010BA7C): DriftManager's board from
	// the top, at most four rows -- each node's car number (names[] index),
	// its score and whether it is the player's.
	int driftRows;
	int driftId[4];
	float driftPts[4];
	bool driftMe[4];
	// A score-only opponent's name (a downhill's, drift_board_name), in place
	// of names[]; NULL for a racer.
	const char* driftName[4];
} HudRaceStats;

// Draws them onto the current citro2d target in the column x..x+w from y
// down: the block retail puts in the canvas's top-right corner, scaled up
// to the column's width.
void hud_draw_race_stats(Hud* h, const HudRaceStats* s, C2D_Font font, float fontUnitPx,
	float x, float y, float w);

// FEngHud::ShowShowcaseReward (SpeedPS.elf 0x80114DF0): the photo shoot's
// "Bank Reward for Magazine: 1,234" (types 1 and 3, the DVD line for 2 and 4)
// queued to zoom in four HUD frames after the HUD is next drawn.
void hud_show_showcase_reward(Hud* h, int magType, int baseReward, int starReward);

// DbgHudNotice's (#334): Out_Run_Message's ZoomIn with "You have the Lead!",
// as the Outrun's lead change starts it.
void hud_debug_outrun(Hud* h);
// #334: 0 = the notices at retail's sizes, 1 = the port's bigger ones (hud.c).
extern float g_hudNoticeScale;
