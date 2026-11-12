#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "carsim.h"
#include "race.h"

// The drift race's scoring (#268 E): DriftManager (Speed.elf 0x8018EB10..
// 0x80190060) and RacingCar::UpdateDriftScore (0x802279D4).
//
// A drift's score is the style accumulator's DRIFT moment (style.c): each
// frame DriftManager rates the slide (its speed and angle), multiplies it by
// the bonus zone, the traffic dodged and the car's level (x1..x5), and
// triggers the moment with it. A crash aborts the drift and drops the level
// to x1. The points the moment banks are the score; UpdateDriftScore keeps
// each lap's share.
//
// Retail gives every RacingCar its own DriftManager when the race is a drift
// (TheRaceParameters +0x44) -- the AI's too, under g_tweakAIDriftOpponents
// (1 on the disc), each on its own accumulator (style_ai_add) and with its
// points multiplied by SP_DriftAIHandicapMultiplier for the race's AI skill.
// So the AI score their own drifts, and the board orders every racer by
// points.

// What the frame gives it: RacingCar::Update's view of a racer's car.
typedef struct {
	float dt;
	float lapExact;     // RacingCar +0x14
	int   maxLap;       // +0x09
	bool  newLap;       // +0x0D
	int   laps;         // the race's (Race +0xBE8)
	bool  wrongWay;     // Player +0x234, as the last frame left it; a human's only
	// The racer is home, by any finish: FinishCar runs ForceFinishAll and a
	// last UpdateDriftScore for a car finished short of the laps (the Hurry!
	// timer's, #464).
	bool  finished;
	// The traffic (the World's cars past the racers): each one's position.
	const float (*traffic)[3];
	int   trafficCount;
} DriftFrame;

// The DriftManager's fields the HUD reads (HUD_Drift, UpdateDriftStunts).
typedef struct {
	bool  on;
	int   level;        // [10], the multiplier, 1..5
	int   nextPoints;   // [0xB], the drift points the next level needs; 0 at x5
	bool  losing;       // [7], LosingMult: too slow or the wrong way, the level dropping
	bool  tooSlow;      // [8], TooSlowToGain
	float bonusZone;    // [9], CalcBonusZoneMult
	float dodge;        // [0x12], CalcDodgeTrafficMult
	float intensity;    // [0x10], CalcDriftIntensity
	float wrongWaySec;  // [0xE], WrongWay_CountDown; < 0 when not shown
	int   kingLevel;    // [0x13], the King drift levels passed in this drift
	float peakYaw;      // [0x14], rad
	bool  drifting;     // the DRIFT moment is ACTIVE
	float points;       // its points, this drift's
	int   score;        // its banked points: the race score
} DriftView;

// Initialize (0x8018EB80) for the player: `carClass` is the ride's
// (ridephys_car_class), `topSpeed` its theoretical top speed, m/s.
// It clears every manager and the board.
void drift_begin(int carClass, float topSpeed);
// An AI racer's manager (Initialize for driver type 2): `id` is its
// race_car() index (1..RACE_MAX_AI), `aiSkill` the race's (0..2), which picks
// the handicap. False outside a drift or with no accumulator for it.
bool drift_ai_add(int id, const CarSim* car, int carClass, float topSpeed, int aiSkill);
void drift_end(void);
bool drift_on(void);

// UpdateDriftScore: DriftManager::Update, then ForceFinishAll once the last
// lap is done, then the lap's share of the score. Run it after style_update.
void drift_update(const CarSim* car, const DriftFrame* f);
// The same for AI racer `id`'s car, its accumulator stepped first
// (style_ai_update), as RacingCar::Update steps it.
void drift_ai_update(int id, const CarSim* car, const DriftFrame* f);

// NotifyPlayerReset (0x8018FC18), from Player::HandleReset.
void drift_player_reset(void);

int   drift_score(void);
// The points scored in lap `lap` (1-based, RacingCar +0x13C), 0 for none.
float drift_lap_score(int lap);
void  drift_view(DriftView* out);
// ShowNiftyDriftyMessage's last name hash (English.bin), taken once; 0 none.
u32   drift_take_nifty(void);

// DriftManager's leaderboard (#268 E step 3): every racer's drift score, the
// highest first. A car's id is its car number, the race_car() index (0 the
// player). It is built on the first update of a drift and kept until the
// next drift begins, so the post-race screens can read it after drift_end.
// On the downhill tracks (4174..4179) it holds the player and three
// score-only opponents instead, ids 100..102, each on the score
// TheDownhillTrackScores gives the player's car class and the AI skill.
#define DRIFT_BOARD_MAX 8
// BuildLeaderBoard's inputs, before the first drift_board_update: the track
// (TheRaceParameters +0), the race's AI skill (+0x78) and the names
// GetRandomCharacterNames drew for the score-only opponents (the first three
// are used).
void drift_board_setup(int track, int aiSkill, const char* const* names, int nameCount);
// UpdateLeaderBoard, from the end of the race's update in a drift; `cars`
// is how many racers the race has, read when the board is first built.
void drift_board_update(int cars);
int  drift_board_count(void);
// GetLeaderBoardPos: the car's place on the board, from 1; 0 when it is not on it.
int  drift_board_pos(int id);
// The k-th node from the top: its car id, score and whether it is a human's.
bool drift_board_at(int k, int* id, float* score, bool* player);
// A score-only opponent's name (ids 100..102), NULL for any other id.
const char* drift_board_name(int id);
// Car `id`'s figures for FinishedRaceStatsEntry::Construct: RacingCar +0x13C
// and its accumulator's DRIFT moment, with its board place. False (the place
// still set) for a car with no manager, or outside a drift.
bool drift_car_stats(int id, RaceDriftCar* out);
