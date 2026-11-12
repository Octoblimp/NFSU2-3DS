// The race replay (TASKS.md #498): Pause > Replay and the post-race Replay,
// ReplayMenu's UI_ReplayControl.fng over a recording of the race.
//
// Retail (ReplayManager, Speed.elf 0x80199154..; SPEED2.EXE the same) keeps
// full-world snapshots -- DoSnapshot of every car, the AI and the timers --
// and replays the joystick log on from one: the world is deterministic, so
// running it again from the snapshot is the replay. MarkTheBeginningOfTime
// starts it only in a race (TheRaceParameters+0x14 game mode not 0 or 7, and
// not two humans); free roam has none, as here.
// PORT: the port's world is not deterministic (a variable step, collision
// streamed in on demand), so it records what is drawn and heard instead --
// every car's pose, wheels and engine at REPLAY_HZ -- and plays that back
// into the live cars with the simulation stopped, the way an overlay stops
// it. What retail's snapshots are for here is the timeline: the instant
// replay (every 300 world frames, two kept, the older one where Pause >
// Replay starts) and the lap snapshots (MaybeTakeLapSnapshot: one every
// 15 s, five kept, the interval doubling and every other one going when they
// are full) are kept as marks, and those are ReplayMenu's jump points.
//
// The recording lives in the malloc heap in REPLAY_BLOCK blocks, at most
// REPLAY_BUDGET bytes, and never below REPLAY_HEAP_FLOOR free. When it is
// full the frames are thinned to every other one and the rate halves --
// MaybeTakeLapSnapshot's own way of fitting a race into what it has.
#ifndef REPLAY_H
#define REPLAY_H

#include <3ds.h>
#include <stdbool.h>
#include "carsim.h"
#include "traffic.h"

#define REPLAY_FULL_MAX     8     // the player and the field
#define REPLAY_TRAFFIC_MAX  16
#define REPLAY_MARKS_MAX    10    // REPLAY_TIME_1..10

// One car the recording follows: the player (slot 0) and the field. The
// pointers are the live car's; any but `sim` may be NULL.
typedef struct {
	CarSim* sim;
	bool*   simReady;   // Opponent: in the world at all
	bool*   inactive;
	float*  blinkSec;   // the blink after a reset
	float*  lastThr;    // Opponent: the throttle its sound reads
} ReplayCarRef;

// What happened between two frames that only the world step sees: the
// player's backfire and bottom-out (SFXMessage 9 / 5).
typedef struct {
	bool backfire;
	int  bottomOut;     // -1 none, else 0..127
} ReplayEvents;

// Per frame, from the race: the player's throttle and its lap (-1 before
// the start line).
typedef struct {
	float throttle;
	int   lap;
	s32   lapTicks;     // the lap clock (race_clock less the last start-line crossing)
	bool  backfire;
	int   bottomOut;
} ReplayFrameIn;

// A new race (or none): everything recorded goes. `full` is copied.
void  replay_reset(void);
void  replay_bind(const ReplayCarRef* full, int nFull, Traffic* traffic);
bool  replay_bound(void);
// One world step of `dt` seconds has run: record when the rate says so.
void  replay_record(float dt, const ReplayFrameIn* in, void (*log)(const char*));

// What there is: at least a second of race.
bool  replay_has_data(void);
float replay_start(void);         // the first frame's time (0)
float replay_end(void);           // the last frame's
int   replay_frames(void);
u32   replay_bytes(void);
float replay_rate(void);          // frames a second now

// ReplayMenu's list: the lap snapshots and the older instant replay, by
// time, a lap one dropped within 20 s of the end (the first kept).
int   replay_marks(float* out, int max);

// Playback. capture_present keeps every bound car as it is now (THE_PRESENT);
// apply poses them as they were at `t`; restore_present puts them back.
bool  replay_capture_present(void);
void  replay_restore_present(void);
void  replay_apply(float t);
// The events of the frames in (t0, t1], and the race's per-frame values at t.
void  replay_events(float t0, float t1, ReplayEvents* ev);
float replay_throttle_at(float t);
int   replay_lap_at(float t);
s32   replay_lap_ticks_at(float t);

#endif
