// The envelopes the sound controllers share: cInterpLine, cPathLine and
// Average, Speed.elf's audio helpers. enginectl.c's SFXCTL objects and
// carsfx.c's CARSFX objects both run them, once a frame, with the frame's dt.
#pragma once

// cInterpLine (ctor 0x800850CC, Initialize 0x80085128, Update 0x800851F0 and
// 0x800851A4): a value eased from `start` to `end` over `dur` seconds.
typedef struct {
	float elapsed;  // +0x00
	float dur;      // +0x04
	float start;    // +0x08
	float end;      // +0x0C
	int   curve;    // +0x10
	float cur;      // +0x14
	int   done;     // +0x18
} InterpLine;

// cPathLine (Initialize 0x80084CEC, ClearStages 0x80084D50, AddStage
// 0x80084DA8, Update 0x80084E9C): up to five such lines run back to back.
typedef struct {
	float elapsed;   // [0x00]
	float dur[6];    // [0x01]
	float start[6];  // [0x07]
	float end[6];    // [0x0D]
	int   link[6];   // [0x13] start where the last stage ended; AddStage clears it, nothing sets it
	int   curve[6];  // [0x19]
	int   count;     // [0x1F]
	int   stage;     // [0x20]
	float cur;       // [0x21] +0x84
	int   done;      // [0x22] +0x88
} PathLine;

// Average (ctor 0x801A5494, Record 0x801A571C, Flush 0x801A5834, Recalculate
// 0x801A5680): the mean of the last `size` values.
#define AVERAGE_MAX 15
typedef struct {
	int   size;     // +0x01
	int   count;    // +0x02
	int   idx;      // +0x03
	float sum;      // +0x08
	float avg;      // +0x0C
	// +0x14, in place for sizes up to 5 (Init 0x801A54F8); retail allocates a
	// longer one, which the port keeps in place too, up to 15 (the AI's
	// steering monitor, TWK_SND_SteeringMonitor).
	float data[AVERAGE_MAX];
} Average;

float ease(int curve, float e, float d, float a, float b);
void lineIdle(InterpLine* l);
void lineInit(InterpLine* l, float a, float b, int ms, int curve);
void lineUpdate(InterpLine* l, float dt);
void lineTrack(InterpLine* l, float dt, float target);
void pathClear(PathLine* p);
void pathAdd(PathLine* p, float a, float b, int ms, int curve);
void pathInit(PathLine* p, float a, float b, int ms);
void pathUpdate(PathLine* p, float dt);
void avgInit(Average* a, int size);
void avgRecord(Average* a, float v);
void avgFlush(Average* a, float v);
void avgRecalc(Average* a);
