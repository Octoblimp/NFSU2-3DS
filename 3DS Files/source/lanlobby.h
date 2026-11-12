// lanlobby -- the LAN lobby over local wireless, the port's (TASKS #321b).
//
// The PC's LAN play (docs/formats/lan-lobby-pc-re.md) has a server, which
// one PC hosts ("Host LAN Server", UI_PC_LAN_ServerSelect), and on it a
// lobby of games ("LAN Lobby", PC_OL_Lobby): a player creates a game
// through the Quick Race screens, the others join it, each game has its
// room (UI_OLGameRoom: six slots, each with its ready light), and the
// game's creator starts it. On the PC, server.dll keeps the games and
// every PC talks to it over UDP.
//
// Here the server is the console that hosts the UDS network (wireless.h),
// six consoles at most, itself among them. The network is a star, so the
// server already hears every console: it keeps the games and the names,
// answers each request, and sends every console the lists whenever they
// change. A console's own requests, the server's among them, go through
// the same handler, so the server plays as any other console does.
//
// No <3ds.h>: the link comes in through LanNet, which main.c fills with
// wireless.c's calls, and the state is a LanLobby, so the host check
// (tools/lanlobby_check.py) runs six consoles in one process. The race's
// own setup and lockstep (#321c) take over at LAN_EV_GO.
//
// Console ids: the server is 0, its wireless peer i is i + 1. A client
// learns its id from the server's welcome. wireless.c keeps a peer's place
// when it goes: a console that comes takes a new place while there are
// fewer than five, then the first of one gone, so the ids stay put; the
// lobby never calls wireless_forget_lost, which would move them. A place
// taken again says so with its hello, and is a new console.

#ifndef LANLOBBY_H
#define LANLOBBY_H

#include <stddef.h>
#include <stdint.h>

#include "wireless.h"

#define LAN_CONSOLES     WIRELESS_MAX_CONSOLES   // 6
#define LAN_MAX_GAMES    LAN_CONSOLES            // a game has its creator in it
#define LAN_NAME_MAX     16                      // the PC's 16-byte slots: 12 typed
#define LAN_NAME_TYPED   12                      // FUN_004a2ff0's limit
#define LAN_PASSWORD_MAX 13                      // 3..12 characters and the end
#define LAN_COUNTDOWN_MS 5000                    // PORT: the PC's is the session's
#define LAN_SCAN_MAX     WIRELESS_MAX_HOSTS

// Message types on the link: 0x20..0x3f are the lobby's.
enum {
    LAN_MSG_HELLO   = 0x20,  // client: its name
    LAN_MSG_WELCOME = 0x21,  // server: the client's id, the server's name
    LAN_MSG_NAMES   = 0x22,  // server: every console's name
    LAN_MSG_GAMES   = 0x23,  // server: every game
    LAN_MSG_CREATE  = 0x24,  // client: a game, its settings and password
    LAN_MSG_JOIN    = 0x25,  // client: a game's id and the password typed
    LAN_MSG_LEAVE   = 0x26,  // client: out of its game
    LAN_MSG_READY   = 0x27,  // client: ready (1) or not (0)
    LAN_MSG_START   = 0x28,  // the creator: start the game
    LAN_MSG_RESULT  = 0x29,  // server: a request's answer
    LAN_MSG_GO      = 0x2a,  // server: the game is on: its seed and racers
    LAN_MSG_CAR     = 0x2b,  // client: the car it chose (Select Car)
    LAN_MSG_KICK    = 0x2c,  // the creator: a racer out ("Kicking %s")
};

// A game's settings: the PC's session block at 0x850080, as much of it as
// LAN uses (lan-lobby-pc-re.md, section 1). Ranked and private are never
// on in LAN (FUN_004b41c0), so they are not here.
typedef struct LanSettings {
    uint8_t  maxPlayers;     // 0x850080: 2..6 in every mode (PORT, the user
                             // 2026-11-09; the PC's 2..4, URL 2..6)
    uint8_t  mode;           // 0x850088: 0 circuit, 1 sprint, 2 drag, 3 drift,
                             // 4 URL, 8 outrun, 10 Street X
    uint16_t track;          // 0x8500b8
    uint8_t  reverse;        // 0x8500bc
    uint8_t  laps;           // 0x8500ba
    uint8_t  restriction;    // 0x85008c: Evenly Matched (1) or none (0)
    uint8_t  raceBreaker;    // 0x850099
    uint8_t  collision;      // 0x85009a
    uint8_t  outrunPoints;   // 0x850090, an index
    uint8_t  outrunTime;     // 0x850094, an index
    uint8_t  locked;         // 0x85009b: a password is set
} LanSettings;

#define LAN_SETTINGS_WIRE 13

typedef struct LanGame {
    uint8_t     id;          // 1..255, 0 for none
    uint8_t     creator;     // console id
    uint8_t     members;     // a bit per console id
    uint8_t     ready;       // a bit per console id; the creator's always set
    uint8_t     started;     // GO sent: no one joins
    uint8_t     countdown;   // seconds left, 0 when none runs
    LanSettings set;
} LanGame;

// Join's and create's answers: the PC's join errors (lan-lobby-pc-re.md,
// "Join Game").
typedef enum LanResult {
    LAN_OK = 0,
    LAN_FULL,                // the game has its max players
    LAN_WRONG_PASSWORD,
    LAN_STARTED,             // GO already sent
    LAN_INVALID_CAR,         // Evenly Matched and the car is not in its class
    LAN_GONE,                // no such game any more
    LAN_BUSY,                // already in a game, or no room for another
    LAN_NOT_ENOUGH,          // start: fewer than two racers once the
                             // unready are dropped (0xd616e55a)
    LAN_NOT_CREATOR,         // start or kick: only the creator does
    LAN_REMOVED,             // dropped as unready, or kicked (0x7d162708)
} LanResult;

typedef enum LanEventKind {
    LAN_EV_NONE = 0,
    LAN_EV_SERVER_UP,        // hosting, or welcomed by the server
    LAN_EV_SERVER_LOST,      // the server went (a client's), or the network
    LAN_EV_LISTS,            // the names or the games changed
    LAN_EV_ENTERED,          // into a game (created or joined): `game`
    LAN_EV_REFUSED,          // a create, join or start refused: `result`
    LAN_EV_CLOSED,           // out of the game this console was in:
                             // `result` LAN_GONE (it closed) or LAN_REMOVED
    LAN_EV_GO,               // the game is on: lan_go() has it
} LanEventKind;

typedef struct LanEvent {
    LanEventKind kind;
    uint8_t      game;
    LanResult    result;
} LanEvent;

// The race the server started, as every racer gets it.
typedef struct LanGo {
    uint8_t     game;
    uint32_t    seed;
    int         racers;                  // 2..6
    uint8_t     console[LAN_CONSOLES];   // grid order: the creator first
    LanSettings set;
} LanGo;

// A server in range, from the scan.
typedef struct LanServer {
    char name[LAN_NAME_MAX];
    int  consoles, maxConsoles;
    int  scanIndex;
} LanServer;

typedef struct LanNet {
    void         *ctx;
    int           (*start)(void *ctx);
    void          (*stop)(void *ctx);
    int           (*host)(void *ctx, int consoles, const void *appdata, size_t n);
    int           (*scan)(void *ctx, WirelessHost *out, int max);
    int           (*join)(void *ctx, int scanIndex);
    void          (*leave)(void *ctx);
    WirelessState (*state)(void *ctx);
    int           (*peerCount)(void *ctx);
    int           (*peerAlive)(void *ctx, int i);
    int           (*send)(void *ctx, int peer, uint8_t type, const void *data, size_t n);
    int           (*recv)(void *ctx, int peer, WlMsg *out);
} LanNet;

typedef void (*LanLogFn)(const char *line);

// The race's messages (0x30 on, lanrace.h): passed on as they come, the
// console they came from first. The lobby reads none of them.
#define LAN_MSG_OTHER_FIRST 0x30
typedef void (*LanOtherFn)(void *ctx, int console, uint8_t type,
                           const uint8_t *d, size_t n);

#define LAN_EVENTS 16

typedef struct LanLobby {
    const LanNet *net;
    LanLogFn      log;
    int           on;                       // a server hosted or joined
    int           server;                   // this console is the server
    int           self;                     // this console's id, -1 unknown
    char          selfName[LAN_NAME_MAX];
    char          serverName[LAN_NAME_MAX];
    uint32_t      now;

    // The lists: the server's own, a client's as last sent.
    char          name[LAN_CONSOLES][LAN_NAME_MAX];
    uint8_t       present;                  // a bit per console id: its hello in
    LanGame       games[LAN_MAX_GAMES];
    int           numGames;
    uint8_t       inGame;                   // this console's game, 0 none

    // The server's alone.
    char          password[LAN_MAX_GAMES][LAN_PASSWORD_MAX];
    uint32_t      car[LAN_CONSOLES];        // each console's car's class
    uint8_t       carSet;                   // a bit per console id
    uint32_t      countdownEnd[LAN_MAX_GAMES];  // 0: none runs
    uint8_t       nextId;
    uint32_t      seedMix;
    int           dirty;                    // the lists go out this frame

    LanGo         go;
    LanOtherFn    other;                    // lanlobby_set_other
    void         *otherCtx;
    int           racing;                   // the server's: a race runs
    LanEvent      ev[LAN_EVENTS];
    int           evHead, evCount;
} LanLobby;

void lanlobby_init(LanLobby *L, const LanNet *net, LanLogFn log);

// Host LAN Server: UDS up, a network of six, this console the server.
// `name` is the one UI_PC_LAN takes, the server's and the player's.
int  lanlobby_host(LanLobby *L, const char *name, uint32_t nowMs);
// The servers in range: how many, -1 when the scan failed.
int  lanlobby_scan(LanLobby *L, LanServer *out, int max);
// Joins the server the scan listed (its scanIndex), as `name`.
int  lanlobby_join_server(LanLobby *L, int scanIndex, const char *name,
                          uint32_t nowMs);
// Off the server, and UDS down.
void lanlobby_leave_server(LanLobby *L);

// Once a frame: the messages in, the server's answers, its countdowns and
// lists out.
void lanlobby_frame(LanLobby *L, uint32_t nowMs);

// This console's requests; the answer comes as an event.
void lanlobby_create(LanLobby *L, const LanSettings *s, const char *password);
void lanlobby_join_game(LanLobby *L, uint8_t game, const char *password);
void lanlobby_leave_game(LanLobby *L);
void lanlobby_set_ready(LanLobby *L, int ready);
void lanlobby_start(LanLobby *L);
void lanlobby_kick(LanLobby *L, int console);
// The car's class, for Evenly Matched (0: none chosen).
void lanlobby_set_car(LanLobby *L, uint32_t carClass);
// Who takes the race's messages (lanrace_message), after lanlobby_init.
void lanlobby_set_other(LanLobby *L, LanOtherFn fn, void *ctx);
// The server's: a race runs (1) or none (0). While one runs, a game's
// start is refused as LAN_BUSY: one race at a time (lanrace.h).
void lanlobby_set_racing(LanLobby *L, int racing);

// The next event: 1, or 0 for none.
int  lanlobby_event(LanLobby *L, LanEvent *out);

const LanGame *lanlobby_game(const LanLobby *L, uint8_t id);
const LanGo   *lanlobby_go(const LanLobby *L);

// The settings as UI_QRModeOptions would leave them, for `mode`.
void lanlobby_default_settings(LanSettings *s, uint8_t mode, uint16_t track,
                               int pointToPoint);
// The PC's lap range (0x4B3E40): min and max for the mode; a point-to-
// point track (track +0x80 == 1) has one lap.
void lanlobby_lap_range(uint8_t mode, int pointToPoint, int *lo, int *hi);

#endif
