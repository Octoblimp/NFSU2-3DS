// lanrace -- a LAN race: the racers' cars, the start and each car's state
// as it races, the port's (TASKS #321c).
//
// The PC's LAN race (server.dll over UDP) sends each car's state to the
// others, and each PC draws the others' cars from it; its game room's
// Collision option says whether they can be hit. The port does the same.
// Each console drives its own car, as in a Quick Race, and sends that car's
// state every frame. The others' cars take the field's places. Each is a
// puppet: carsim drives it on its owner's inputs between states, and every
// state that comes in pulls it back onto its owner's pose (main.c).
//
// After lanlobby's GO (LAN_EV_GO), a racer's console sends the server its
// car: LanRaceCar, what the others need to build and dress it. Once every
// racer's car is in, the server sends every racer the table, in grid order,
// and each console loads the race (LANRACE_EV_CARS). With its race loaded
// and on the grid, a console says so. Once every racer has, the server
// starts the countdown on all of them (LANRACE_EV_START). From then on each
// state goes to the server, unreliable, and the server sends it on to the
// other racers. Finishes and leavers go reliably the same way.
//
// The server need not race: it keeps the lobby, so it relays every race
// from its menus as well as from its own race (lanrace_frame from both).
// One race runs at a time; lanlobby refuses another game's start while one
// does (lanlobby_set_racing).
//
// A racer lost (its link gone, or the server's patience out while the cars
// or the loads are waited for) leaves the race: the server tells the
// others (LANRACE_EV_LEFT) and its car goes.
//
// No <3ds.h>: the link comes in through LanRaceNet, which main.c fills
// from lannet.c, and the messages through lanlobby's "other" hook, so the
// host check (tools/lanrace_check.py) runs six consoles in one process.

#ifndef LANRACE_H
#define LANRACE_H

#include <stddef.h>
#include <stdint.h>

#include "lanlobby.h"

#define LANRACE_RIDE_SLOTS   0xAB     // carpartdb.h's CARPARTDB_SLOTS + 1
#define LANRACE_CAR_WIRE_MAX (17 + 5 * LANRACE_RIDE_SLOTS)
#define LANRACE_TABLE_MAX    (1 + LAN_CONSOLES + LAN_CONSOLES * (2 + LANRACE_CAR_WIRE_MAX))
#define LANRACE_PATIENCE_MS  60000    // cars and loads waited for at most
#define LANRACE_STATE_WIRE   76
#define LANRACE_EVENTS       16

// Message types: 0x30.. are the race's (lanlobby passes them on).
enum {
    LANRACE_MSG_LOADED = 0x32,  // a racer: its race loaded, on the grid
    LANRACE_MSG_CAR    = 0x33,  // a racer: a piece of its car
    LANRACE_MSG_CARS   = 0x34,  // the server: a piece of the cars' table
    LANRACE_MSG_START  = 0x35,  // the server: the countdown, on every racer
    LANRACE_MSG_STATE  = 0x36,  // unreliable: a car's state, its console's
    LANRACE_MSG_FINISH = 0x37,  // a car finished: console, ms since START
    LANRACE_MSG_LEFT   = 0x38,  // a racer left the race: console
    LANRACE_MSG_LEAD   = 0x39,  // the server: an Outrun's leader, console, ms
    LANRACE_MSG_OUT    = 0x3A,  // the server: out of an Outrun, console, ms, won
};

// What a racer races, for the others to build and dress (opponent.h's
// Opponent: carType, ride, camber, the sound levels).
typedef struct LanRaceCar {
    uint32_t carType;       // the car table record
    uint32_t nameHash;      // its CarRec nameHash, checked on the way in
    float    camber;        // GetCamberPercent of its build
    uint8_t  sndEng, sndTrans, sndTurbo, nosUpgrade;
    uint32_t ride[LANRACE_RIDE_SLOTS];  // [0] the car's hash, [slot + 1]
                                        // each part's; 0 for none
} LanRaceCar;

// A car's state as its console last sent it.
#define LANRACE_ST_HANDBRAKE 0x01
#define LANRACE_ST_NITROUS   0x02
#define LANRACE_ST_BLINKING  0x04   // fresh off a reset: no contact
#define LANRACE_ST_FINISHED  0x08
typedef struct LanRaceState {
    uint16_t seq;           // the sender's frames since START
    uint32_t ms;            // the sender's ms since START
    float    pos[3];        // the rigid body's (CarSim rbPos)
    float    fwd[3], up[3]; // its rows 0 and 2
    float    vel[3], w[3];  // world m/s, rad/s
    uint16_t steerU16;      // the wheels' angle, 65536 to the turn
    uint8_t  thr, brake;    // 0..255
    uint8_t  flags;         // LANRACE_ST_*
    int8_t   gear;
    uint16_t rpm;
    uint8_t  lap;
} LanRaceState;

typedef struct LanRaceNet {
    void *ctx;
    // To a console (the server's own id 0 for a client); 1 when queued, 0
    // when the window is full or there is no such console.
    int  (*send)(void *ctx, int console, uint8_t type, const void *data,
                 size_t n, int reliable);
    int  (*alive)(void *ctx, int console);
} LanRaceNet;

typedef enum LanRaceStage {
    LANRACE_OFF = 0,
    LANRACE_CARS,           // the cars being gathered (the server) or the
                            // table waited for (a racer)
    LANRACE_LOADING,        // the table in: this console loads the race
    LANRACE_HOLD,           // loaded and on the grid, START waited for
    LANRACE_RACING,
} LanRaceStage;

typedef enum LanRaceEvKind {
    LANRACE_EV_NONE = 0,
    LANRACE_EV_CARS,        // the table is in: load the race
    LANRACE_EV_START,       // the countdown
    LANRACE_EV_FINISH,      // `console` finished, finishMs[console]
    LANRACE_EV_LEFT,        // `console` left the race
    LANRACE_EV_ABORT,       // no race: too few racers left before START
    LANRACE_EV_LEAD,        // an Outrun: `console` has the lead
    LANRACE_EV_OUT,         // an Outrun: `console` is out (otrOutMs), or won
                            // it (otrWon)
} LanRaceEvKind;

typedef struct LanRaceEvent {
    LanRaceEvKind kind;
    uint8_t       console;
} LanRaceEvent;

typedef struct LanRaceOut {     // a reliable message in pieces, under way
    uint8_t  type;
    uint16_t off, n;
    const uint8_t *d;
} LanRaceOut;

typedef struct LanRace {
    const LanRaceNet *net;
    LanLogFn      log;
    LanRaceStage  stage;
    int           server;           // this console is the LAN server
    int           self;             // this console's id
    int           racing;           // this console races
    uint32_t      now, since;       // ms; `since` the stage's start
    int           racers;
    uint8_t       console[LAN_CONSOLES];   // grid order
    uint8_t       in;               // the racers still in, a bit per console

    LanRaceCar    car[LAN_CONSOLES];      // by console id
    uint8_t       carIn;                  // a bit per console
    // The server's: each racer's car as it came; this console's own.
    uint8_t       carWire[LAN_CONSOLES][LANRACE_CAR_WIRE_MAX];
    uint16_t      carWireLen[LAN_CONSOLES];
    uint16_t      carWireGot[LAN_CONSOLES];
    // The table: the server's, built; a racer's, as it comes.
    uint8_t       table[LANRACE_TABLE_MAX];
    uint16_t      tableLen, tableGot;
    LanRaceOut    out[LAN_CONSOLES];      // by console id
    uint8_t       loaded;                 // the server's: a bit per console

    LanRaceState  st[LAN_CONSOLES];       // the cars' last, by console
    uint8_t       stIn;                   // a bit per console: one came
    uint32_t      stAt[LAN_CONSOLES];     // when, ms
    uint16_t      seq;                    // this console's frames since START
    uint32_t      finishMs[LAN_CONSOLES];
    uint8_t       finished;               // a bit per console

    LanRaceEvent  ev[LANRACE_EVENTS];
    int           evHead, evCount;

    // An Outrun (the room's mode 8, #488): the server's lead game, as the
    // PC's (0x5F9CD0) is the server's.
    uint8_t       outrun;
    uint32_t      otrLimitMs;             // the room's time limit, 0 none
    uint32_t      otrStart;               // the server's START, ms
    float         otrDist;                // GetGetAwayDistance, m
    int8_t        leader;                 // console, -1 none yet
    uint32_t      leadCool;               // the server's: no change before
    uint8_t       otrOut;                 // a bit per console: out, or won
    uint8_t       otrWon;                 // the winner's console, 0xff none
    uint32_t      otrOutMs[LAN_CONSOLES]; // since START
    // The server's: what each racer has been told (a reliable send can find
    // the window full, so what is missing goes again the next frame).
    uint8_t       otrLeadTold[LAN_CONSOLES];   // leader + 1, 0 none
    uint8_t       otrOutTold[LAN_CONSOLES];    // a bit per console
} LanRace;

void lanrace_init(LanRace *R, const LanRaceNet *net, LanLogFn log);

// lanlobby's GO on this console: `self` its id, `server` whether it is the
// LAN server, `car` its own car when it races (NULL when the server's
// console is not in the game).
void lanrace_begin(LanRace *R, const LanGo *go, int self, int server,
                   const LanRaceCar *car, uint32_t nowMs);
// lanlobby's "other" hook: a race message from `console`.
void lanrace_message(LanRace *R, int console, uint8_t type,
                     const uint8_t *d, size_t n);
// Once a frame, from the LAN screens and the race: pieces out, the waits.
void lanrace_frame(LanRace *R, uint32_t nowMs);

// This console's race is loaded and its cars on the grid.
void lanrace_loaded(LanRace *R);
// This console's car this frame, once racing.
void lanrace_put_state(LanRace *R, const LanRaceState *s);
// This console's car finished, `ms` after START.
void lanrace_finished(LanRace *R, uint32_t ms);
// This console leaves the race (quit, or back to the lobby).
void lanrace_leave(LanRace *R);
// An Outrun's GetGetAwayDistance, m: 300 metric, else 1000 ft (the server's).
void lanrace_outrun_distance(LanRace *R, float m);

int  lanrace_event(LanRace *R, LanRaceEvent *out);
// A racer's place on the grid (0 pole), -1 not racing.
int  lanrace_grid(const LanRace *R, int console);

void lanrace_encode_car(const LanRaceCar *c, uint8_t *b, uint16_t *n);
int  lanrace_decode_car(LanRaceCar *c, const uint8_t *b, uint16_t n);
void lanrace_encode_state(const LanRaceState *s, int console, uint8_t *b);
int  lanrace_decode_state(LanRaceState *s, int *console, const uint8_t *b,
                          size_t n);

#endif
