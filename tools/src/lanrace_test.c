// Builds "3DS Files/source/lanrace.c" and runs up to six consoles of it in
// one process over a fake star (TASKS #321c): the server, console 0, and
// its clients, each link a FIFO of reliable messages with a window (as
// wlproto's) and a lossy channel for the unreliable ones. Each scenario
// begins a race (lanlobby's GO), lets each racer load after its own time,
// runs the race with every racer sending its state each frame, and checks:
//
//   - every racer gets the cars' table once, every racer's car in it as it
//     sent it, and in grid order;
//   - START reaches every racer still in, never before the last one in has
//     loaded;
//   - each racer hears every other racer's states, as they were sent, the
//     server racing or not;
//   - finishes reach every racer still in, with their time;
//   - a racer lost, or late with its car or its load past the patience,
//     leaves the race on every console; fewer than two before START is no
//     race (LANRACE_EV_ABORT); a racer's own leave is told to the rest;
//   - nothing goes over a link that is down, and no message is longer than
//     WL_MSG_MAX;
//   - an Outrun (#488, the room's mode 8): the server's lead game -- the pole
//     leads, a racer 305 m back is out, one that passes within 30 m takes
//     the lead, the last one near wins, the time limit ends it -- told to
//     every racer the same, through a lossy link and a small window.
//
// Prints "faults N" at the end; tools/lanrace_check.py runs it.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lanrace.h"

#define MAXC   LAN_CONSOLES
#define STEP   16          // ms a frame
#define QMAX   4096

static int g_faults, g_verbose;
static const char *g_scen;

static void fault(const char *fmt, ...)
{
    va_list ap;
    printf("  FAULT [%s] ", g_scen);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    g_faults++;
}

typedef struct Msg {
    int      from, to, reliable;
    uint32_t at;           // delivered from this time on
    uint8_t  type;
    uint16_t n;
    uint8_t  d[WL_MSG_MAX];
} Msg;

static Msg      g_q[QMAX];
static int      g_qn;
static uint32_t g_now;
static int      g_up[MAXC];        // client c's link to the server
static int      g_window = 8;       // reliable in flight on a link, at most
static int      g_lossPct;
static int      g_latency = STEP;
static uint32_t g_rng = 1;

static uint32_t lcg(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

typedef struct Con {
    LanRace      R;
    LanRaceNet   net;
    int          id;
    int          racing;
    LanRaceCar   car;
    // what it saw
    int          cars, starts, aborts;
    uint32_t     startAt;
    uint8_t      leftSeen, finSeen;
    uint32_t     finMs[MAXC];
    int          loadAt;           // ms after its table that it loads, -1 never
    uint32_t     tableAt;
    int          loadedSaid;
    uint16_t     lastSeq[MAXC];
    int          stSeen[MAXC];
    int          finishAfter;      // frames racing before it finishes, 0 never
    int          leaveAfter;       // frames racing before it leaves, 0 never
    int          racedFrames;
    int          gone;             // left, or lost
} Con;

static Con C[MAXC];
static int g_cons;

static int in_flight(int from, int to)
{
    int i, n = 0;
    for (i = 0; i < g_qn; i++)
        if (g_q[i].reliable && g_q[i].from == from && g_q[i].to == to) n++;
    return n;
}

static int link_up(int a, int b)
{
    const int client = a == 0 ? b : a;
    return client > 0 && client < g_cons && g_up[client];
}

static int fake_send(void *ctx, int console, uint8_t type, const void *data,
                     size_t n, int reliable)
{
    const int from = *(int *)ctx;
    Msg      *m;

    if (from != 0 && console != 0) {
        fault("console %d sends to console %d, not the server", from, console);
        return 0;
    }
    if (console < 0 || console >= g_cons || console == from) {
        fault("console %d sends to no console %d", from, console);
        return 0;
    }
    if (n > WL_MSG_MAX) {
        fault("console %d sends %u bytes", from, (unsigned)n);
        return 0;
    }
    if (!link_up(from, console)) return 0;
    if (reliable && in_flight(from, console) >= g_window) return 0;
    if (g_qn == QMAX) { fault("queue full"); return 0; }
    if (!reliable && (int)(lcg() % 100) < g_lossPct) return 1;
    m = &g_q[g_qn++];
    m->from = from; m->to = console; m->reliable = reliable;
    m->at = g_now + (uint32_t)g_latency;
    m->type = type; m->n = (uint16_t)n;
    memcpy(m->d, data, n);
    return 1;
}

static int fake_alive(void *ctx, int console)
{
    const int from = *(int *)ctx;
    return link_up(from, console);
}

static void logline(const char *line)
{
    if (g_verbose) printf("    %s\n", line);
}

static void deliver(void)
{
    int i, k = 0;
    Msg m;

    for (i = 0; i < g_qn; i++) {
        m = g_q[i];
        if (m.at > g_now) { g_q[k++] = m; continue; }
        if (!link_up(m.from, m.to)) continue;      // lost with the link
        if (!C[m.to].gone || m.to == 0)
            lanrace_message(&C[m.to].R, m.from, m.type, m.d, m.n);
    }
    g_qn = k;
}

static void make_car(LanRaceCar *c, int id)
{
    int k;
    memset(c, 0, sizeof *c);
    c->carType  = (uint32_t)(3 + id * 7);
    c->nameHash = 0xC0DE0000u + (uint32_t)id;
    c->camber   = 0.5f * (float)id;
    c->sndEng = (uint8_t)id; c->sndTrans = 1; c->sndTurbo = 2;
    c->nosUpgrade = (uint8_t)(id & 3);
    for (k = 0; k < LANRACE_RIDE_SLOTS; k++)
        if ((k * 7 + id) % 3 == 0) c->ride[k] = 0x10000u * (uint32_t)id + (uint32_t)k + 1;
}

static void make_state(LanRaceState *s, int id, uint16_t seq)
{
    int k;
    memset(s, 0, sizeof *s);
    s->seq = seq;
    s->ms  = (uint32_t)seq * STEP;
    for (k = 0; k < 3; k++) {
        s->pos[k] = (float)(id * 100 + seq) + 0.25f * (float)k;
        s->fwd[k] = (float)k; s->up[k] = (float)(2 - k);
        s->vel[k] = (float)seq * 0.5f; s->w[k] = -(float)k;
    }
    s->steerU16 = (uint16_t)(seq * 31u + (unsigned)id);
    s->thr = (uint8_t)seq; s->brake = (uint8_t)(255 - seq);
    s->flags = (uint8_t)(seq & 0x0f);
    s->gear = (int8_t)(seq % 6 - 1);
    s->rpm = (uint16_t)(1000 + seq);
    s->lap = (uint8_t)(seq / 100);
}

static int same_state(const LanRaceState *a, const LanRaceState *b)
{
    int k;
    for (k = 0; k < 3; k++)
        if (a->pos[k] != b->pos[k] || a->fwd[k] != b->fwd[k] ||
            a->up[k] != b->up[k] || a->vel[k] != b->vel[k] || a->w[k] != b->w[k])
            return 0;
    return a->seq == b->seq && a->ms == b->ms && a->steerU16 == b->steerU16 &&
           a->thr == b->thr && a->brake == b->brake && a->flags == b->flags &&
           a->gear == b->gear && a->rpm == b->rpm && a->lap == b->lap;
}

typedef struct Scenario {
    const char *name;
    int         consoles;      // 2..6, console 0 the server
    int         serverRaces;
    int         racers;        // in grid order: the racing consoles
    int         loadMs[MAXC];  // ms after its table; -1 never
    int         carLate[MAXC]; // its begin this many ms late; -1 never
    int         lostAt[MAXC];  // ms its link goes; 0 never
    int         finishAfter[MAXC];
    int         leaveAfter[MAXC];
    int         loss, window, latency;
    int         ms;            // how long it runs
    // expected
    int         expectAbort;   // the racers left get ABORT, no START
} Scenario;

static const Scenario S[] = {
    { .name = "two, the server racing", .consoles = 2, .serverRaces = 1,
      .loadMs = {300, 900}, .finishAfter = {200, 260}, .ms = 9000 },
    { .name = "three, the server not racing", .consoles = 3, .serverRaces = 0,
      .loadMs = {0, 500, 100}, .finishAfter = {0, 150, 120}, .ms = 9000 },
    { .name = "six, a lossy link, a small window", .consoles = 6,
      .serverRaces = 1, .loadMs = {100, 2000, 50, 700, 1400, 300},
      .finishAfter = {300, 310, 320, 330, 340, 350}, .loss = 30, .window = 2,
      .latency = 40, .ms = 15000 },
    { .name = "four, a car never sent", .consoles = 4, .serverRaces = 1,
      .loadMs = {100, 200, 300, 400}, .carLate = {0, 0, -1, 0},
      .finishAfter = {100, 100, 0, 100}, .ms = LANRACE_PATIENCE_MS + 12000 },
    { .name = "three, a load never done", .consoles = 3, .serverRaces = 1,
      .loadMs = {100, -1, 300}, .finishAfter = {100, 0, 100},
      .ms = LANRACE_PATIENCE_MS + 12000 },
    { .name = "two, the client lost before START", .consoles = 2,
      .serverRaces = 1, .loadMs = {100, 5000}, .lostAt = {0, 2000},
      .ms = 9000, .expectAbort = 1 },
    { .name = "three, a client lost mid-race", .consoles = 3,
      .serverRaces = 1, .loadMs = {100, 200, 300}, .lostAt = {0, 4000, 0},
      .finishAfter = {400, 0, 410}, .ms = 12000 },
    { .name = "three, the racing server leaves mid-race", .consoles = 3,
      .serverRaces = 1, .loadMs = {100, 200, 300}, .leaveAfter = {60, 0, 0},
      .finishAfter = {0, 300, 320}, .ms = 12000 },
    { .name = "two, the server not racing, one client of three leaves",
      .consoles = 4, .serverRaces = 0, .loadMs = {0, 100, 200, 300},
      .leaveAfter = {0, 0, 50, 0}, .finishAfter = {0, 200, 0, 220}, .ms = 10000 },
};

static void run(const Scenario *sc)
{
    LanGo go;
    int   c, i, racers = 0;
    uint8_t in = 0;

    g_scen = sc->name;
    printf("scenario %s\n", sc->name);
    memset(C, 0, sizeof C);
    g_qn = 0;
    g_now = 1000;
    g_cons = sc->consoles;
    g_lossPct = sc->loss;
    g_window = sc->window ? sc->window : 8;
    g_latency = sc->latency ? sc->latency : STEP;
    for (c = 0; c < MAXC; c++) g_up[c] = c < g_cons;

    memset(&go, 0, sizeof go);
    go.game = 1;
    go.seed = 0x1234;
    for (c = sc->serverRaces ? 0 : 1; c < g_cons; c++) {
        go.console[racers++] = (uint8_t)c;
        in |= (uint8_t)(1u << c);
    }
    go.racers = racers;

    for (c = 0; c < g_cons; c++) {
        Con *k = &C[c];
        k->id = c;
        k->net.ctx = &k->id;
        k->net.send = fake_send;
        k->net.alive = fake_alive;
        lanrace_init(&k->R, &k->net, logline);
        k->racing = (in >> c) & 1;
        make_car(&k->car, c);
        k->loadAt = sc->loadMs[c];
        k->finishAfter = sc->finishAfter[c];
        k->leaveAfter = sc->leaveAfter[c];
    }
    for (c = 0; c < g_cons; c++)
        if (sc->carLate[c] == 0)
            lanrace_begin(&C[c].R, &go, c, c == 0, C[c].racing ? &C[c].car : NULL, g_now);

    for (uint32_t t = 0; t < (uint32_t)sc->ms; t += STEP) {
        g_now += STEP;
        for (c = 1; c < g_cons; c++)
            if (sc->lostAt[c] && t >= (uint32_t)sc->lostAt[c] && g_up[c]) {
                g_up[c] = 0;
                C[c].gone = 1;
            }
        deliver();
        for (c = 0; c < g_cons; c++) {
            Con *k = &C[c];
            LanRaceEvent e;
            if (k->gone && c != 0) continue;
            if (sc->carLate[c] > 0 && t == (uint32_t)sc->carLate[c])
                lanrace_begin(&k->R, &go, c, c == 0, k->racing ? &k->car : NULL, g_now);
            lanrace_frame(&k->R, g_now);
            while (lanrace_event(&k->R, &e)) {
                switch (e.kind) {
                case LANRACE_EV_CARS:
                    k->cars++;
                    k->tableAt = g_now;
                    if (!k->racing) fault("console %d not racing gets the table", c);
                    for (i = 0; i < g_cons; i++) {
                        if (!((k->R.in >> i) & 1)) continue;
                        if (memcmp(&k->R.car[i], &C[i].car, sizeof C[i].car))
                            fault("console %d has console %d's car wrong", c, i);
                    }
                    for (i = 0; i < go.racers; i++)
                        if (k->R.console[i] != go.console[i] && k->R.console[i] != 0xff)
                            fault("console %d's grid %d is %d, not %d", c, i,
                                  k->R.console[i], go.console[i]);
                    break;
                case LANRACE_EV_START:
                    k->starts++;
                    k->startAt = g_now;
                    if (!k->loadedSaid) fault("console %d started unloaded", c);
                    break;
                case LANRACE_EV_FINISH:
                    k->finSeen |= (uint8_t)(1u << e.console);
                    k->finMs[e.console] = k->R.finishMs[e.console];
                    break;
                case LANRACE_EV_LEFT:
                    k->leftSeen |= (uint8_t)(1u << e.console);
                    break;
                case LANRACE_EV_ABORT:
                    k->aborts++;
                    break;
                default:
                    break;
                }
            }
            if (!k->racing || k->gone) continue;
            if (k->cars && !k->loadedSaid && k->loadAt >= 0 &&
                g_now >= k->tableAt + (uint32_t)k->loadAt) {
                k->loadedSaid = 1;
                lanrace_loaded(&k->R);
            }
            if (k->starts && k->R.stage == LANRACE_RACING) {
                LanRaceState s;
                k->racedFrames++;
                make_state(&s, c, (uint16_t)k->racedFrames);
                lanrace_put_state(&k->R, &s);
                if (k->finishAfter && k->racedFrames == k->finishAfter)
                    lanrace_finished(&k->R, (uint32_t)k->racedFrames * STEP + (uint32_t)c);
                if (k->leaveAfter && k->racedFrames == k->leaveAfter) {
                    lanrace_leave(&k->R);
                    k->gone = 1;
                    if (c != 0) k->racing = 0;
                }
                // what it heard of the others
                for (i = 0; i < g_cons; i++) {
                    LanRaceState want;
                    if (i == c || !((k->R.stIn >> i) & 1)) continue;
                    if (k->R.st[i].seq != k->lastSeq[i]) {
                        make_state(&want, i, k->R.st[i].seq);
                        if (!same_state(&want, &k->R.st[i]))
                            fault("console %d has console %d's state %u wrong", c,
                                  i, k->R.st[i].seq);
                        if (k->R.st[i].seq < k->lastSeq[i] && sc->latency == 0)
                            fault("console %d's states from %d went back", c, i);
                        k->lastSeq[i] = k->R.st[i].seq;
                        k->stSeen[i]++;
                    }
                }
            }
        }
    }

    // The tally.
    {
        uint8_t stillIn = 0;   // racing, never lost, never late past patience
        for (c = 0; c < g_cons; c++) {
            const Con *k = &C[c];
            if (!((in >> c) & 1)) continue;
            if (sc->lostAt[c] || sc->carLate[c] < 0 || sc->loadMs[c] < 0) continue;
            stillIn |= (uint8_t)(1u << c);
            (void)k;
        }
        for (c = 0; c < g_cons; c++) {
            const Con *k = &C[c];
            const int expectIn = (stillIn >> c) & 1;
            if (!((in >> c) & 1)) {
                if (k->cars || k->starts) fault("console %d not racing raced", c);
                continue;
            }
            if (!expectIn) {
                if (sc->carLate[c] < 0 || sc->loadMs[c] < 0) {
                    if (k->starts) fault("console %d, late, still started", c);
                    if (!k->aborts && c != 0 && sc->carLate[c] >= 0)
                        fault("console %d, late, never told it is out", c);
                }
                continue;
            }
            if (sc->expectAbort) {
                if (k->aborts != 1) fault("console %d: %d aborts, not 1", c, k->aborts);
                if (k->starts) fault("console %d started an aborted race", c);
                continue;
            }
            if (k->cars != 1) fault("console %d had the table %d times", c, k->cars);
            if (k->starts != 1) fault("console %d had START %d times", c, k->starts);
            for (i = 0; i < g_cons; i++) {
                const int other = (stillIn >> i) & 1;
                const int left = sc->lostAt[i] || sc->leaveAfter[i];
                if (i == c || !((in >> i) & 1)) continue;
                if (other && !left && k->stSeen[i] < 20 && !k->leaveAfter)
                    fault("console %d heard %d states from %d", c, k->stSeen[i], i);
                if (other && sc->finishAfter[i] && !left && !k->leaveAfter) {
                    if (!((k->finSeen >> i) & 1))
                        fault("console %d never heard %d finish", c, i);
                    else if (k->finMs[i] != (uint32_t)sc->finishAfter[i] * STEP + (uint32_t)i)
                        fault("console %d has %d's finish at %lu", c, i,
                              (unsigned long)k->finMs[i]);
                }
                if ((left || !other) && !k->leaveAfter && !((k->leftSeen >> i) & 1))
                    fault("console %d never heard %d leave", c, i);
            }
        }
        // START never before the last one in loaded: every start at or
        // after the latest load time (each racer's table + its load).
        {
            uint32_t lastLoad = 0;
            for (c = 0; c < g_cons; c++)
                if (((stillIn >> c) & 1) && C[c].cars) {
                    const uint32_t l = C[c].tableAt + (uint32_t)C[c].loadAt;
                    if (l > lastLoad) lastLoad = l;
                }
            for (c = 0; c < g_cons; c++)
                if (C[c].starts && C[c].startAt < lastLoad)
                    fault("console %d started at %lu, before the last load at %lu",
                          c, (unsigned long)C[c].startAt, (unsigned long)lastLoad);
        }
    }
    printf("  faults so far %d\n", g_faults);
}

// ------------------------------------------------------------ an Outrun

// Each racer's x along a straight road at frame time `ms` after START (y and
// z 0, heading +x): `plan` picks the script.
static float otr_x(int plan, int c, uint32_t ms, float *v)
{
    const float s = (float)ms * 0.001f;
    if (plan == 0) {
        // 0 leads at 30 m/s; 1 starts 10 m back, 40 m/s from 5 s and passes;
        // 2 at 20 m/s drops back and out; 0 stops at 40 s and drops out.
        if (c == 0) {
            *v = s < 40.0f ? 30.0f : 0.0f;
            return s < 40.0f ? 30.0f * s : 1200.0f;
        }
        if (c == 1) {
            *v = s < 5.0f ? 30.0f : 40.0f;
            return s < 5.0f ? -10.0f + 30.0f * s : 140.0f + 40.0f * (s - 5.0f);
        }
        *v = 20.0f;
        return -20.0f + 20.0f * s;
    }
    // plan 1: all three side by side at 25 m/s; the 2-minute limit ends it.
    *v = 25.0f;
    return 25.0f * s - 5.0f * (float)c;
}

static void run_outrun(const char *name, int plan, int timeIdx, int loss,
                       int window, uint32_t ms, int wantWon,
                       const int *wantOutOrder, int nOut, int wantLeads)
{
    LanGo go;
    int   c, i;
    int   leads[MAXC], outs[MAXC][MAXC], nOuts[MAXC], won[MAXC], wonAfter[MAXC];

    g_scen = name;
    printf("scenario %s\n", name);
    memset(C, 0, sizeof C);
    memset(leads, 0, sizeof leads);
    memset(nOuts, 0, sizeof nOuts);
    g_qn = 0;
    g_now = 1000;
    g_cons = 3;
    g_lossPct = loss;
    g_window = window;
    g_latency = STEP;
    for (c = 0; c < MAXC; c++) g_up[c] = c < g_cons;
    memset(&go, 0, sizeof go);
    go.game = 1;
    go.racers = 3;
    go.set.mode = 8;
    go.set.outrunTime = (uint8_t)timeIdx;
    for (c = 0; c < 3; c++) {
        Con *k = &C[c];
        go.console[c] = (uint8_t)c;
        k->id = c;
        k->net.ctx = &k->id;
        k->net.send = fake_send;
        k->net.alive = fake_alive;
        lanrace_init(&k->R, &k->net, logline);
        k->racing = 1;
        make_car(&k->car, c);
        won[c] = -1;
        wonAfter[c] = -1;
    }
    for (c = 0; c < 3; c++)
        lanrace_begin(&C[c].R, &go, c, c == 0, &C[c].car, g_now);
    for (uint32_t t = 0; t < ms; t += STEP) {
        g_now += STEP;
        deliver();
        for (c = 0; c < 3; c++) {
            Con *k = &C[c];
            LanRaceEvent e;
            lanrace_frame(&k->R, g_now);
            while (lanrace_event(&k->R, &e)) {
                if (e.kind == LANRACE_EV_CARS) k->cars++;
                if (e.kind == LANRACE_EV_START) k->starts++;
                if (e.kind == LANRACE_EV_LEAD) leads[c]++;
                if (e.kind == LANRACE_EV_OUT) {
                    if (k->R.otrWon == e.console) {
                        won[c] = e.console;
                        wonAfter[c] = nOuts[c];
                    } else if (nOuts[c] < MAXC) {
                        outs[c][nOuts[c]++] = e.console;
                    }
                }
            }
            if (k->cars && !k->loadedSaid) {
                k->loadedSaid = 1;
                lanrace_loaded(&k->R);
            }
            if (k->starts && k->R.stage == LANRACE_RACING) {
                LanRaceState s;
                float v;
                memset(&s, 0, sizeof s);
                s.seq = (uint16_t)++k->racedFrames;
                s.ms = (uint32_t)k->racedFrames * STEP;
                s.pos[0] = otr_x(plan, c, s.ms, &v);
                s.pos[1] = (float)c * 2.0f;
                s.vel[0] = v;
                s.fwd[0] = 1.0f;
                s.up[2] = 1.0f;
                lanrace_put_state(&k->R, &s);
            }
        }
    }
    for (c = 0; c < 3; c++) {
        if (won[c] != wantWon)
            fault("console %d has the winner %d, not %d", c, won[c], wantWon);
        if (C[c].R.otrWon != (uint8_t)wantWon)
            fault("console %d's otrWon is %d", c, C[c].R.otrWon);
        if (nOuts[c] != nOut)
            fault("console %d heard %d outs, not %d", c, nOuts[c], nOut);
        for (i = 0; i < nOut && i < nOuts[c]; i++)
            if (outs[c][i] != wantOutOrder[i])
                fault("console %d's out %d is console %d, not %d", c, i,
                      outs[c][i], wantOutOrder[i]);
        // The win last of all: every racer places the cars out from the
        // back, so a win heard first would take a low place.
        if (won[c] >= 0 && wonAfter[c] != nOut)
            fault("console %d heard the win after %d outs, not %d", c, wonAfter[c], nOut);
        if (leads[c] != wantLeads)
            fault("console %d heard %d lead changes, not %d", c, leads[c], wantLeads);
    }
    printf("  faults so far %d\n", g_faults);
}

int main(int argc, char **argv)
{
    size_t i;
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    // The wire both ways.
    {
        LanRaceCar a, b;
        LanRaceState s, t;
        uint8_t buf[LANRACE_CAR_WIRE_MAX], sb[LANRACE_STATE_WIRE];
        uint16_t n;
        int who;
        g_scen = "wire";
        make_car(&a, 5);
        lanrace_encode_car(&a, buf, &n);
        if (n > LANRACE_CAR_WIRE_MAX || !lanrace_decode_car(&b, buf, n) || memcmp(&a, &b, sizeof a))
            fault("a car does not come back as it went");
        for (int k = 0; k < LANRACE_RIDE_SLOTS; k++) a.ride[k] = 0xFFFFFFFFu;
        lanrace_encode_car(&a, buf, &n);
        if (n != LANRACE_CAR_WIRE_MAX || !lanrace_decode_car(&b, buf, n) || memcmp(&a, &b, sizeof a))
            fault("a car with every slot does not come back (%u bytes)", n);
        if (lanrace_decode_car(&b, buf, (uint16_t)(n - 1)))
            fault("a car a byte short is taken");
        make_state(&s, 3, 777);
        lanrace_encode_state(&s, 3, sb);
        if (!lanrace_decode_state(&t, &who, sb, sizeof sb) || who != 3 || !same_state(&s, &t))
            fault("a state does not come back as it went");
        if (lanrace_decode_state(&t, &who, sb, sizeof sb - 1))
            fault("a state a byte short is taken");
    }
    for (i = 0; i < sizeof S / sizeof S[0]; i++) run(&S[i]);
    {
        // 1 takes the lead at ~6 s; 2, 10 m/s slower than 0, is 305 m back
        // of the leader at ~17 s; 1 pulls away from 0 at 10 m/s and 0 is
        // 305 m back at ~36.5 s; 1 is left alone: it wins.
        static const int out0[2] = { 2, 0 };
        static const int out1[2] = { 1, 2 };   // console 0 the leader at the end
        run_outrun("outrun, three: a pass, two out, the last one wins", 0, 0, 0, 8,
                   60000, 1, out0, 2, 2);
        run_outrun("outrun, three, a lossy link and a window of 1", 0, 0, 30, 1,
                   60000, 1, out0, 2, 2);
        run_outrun("outrun, three: the 2-minute limit, the pole wins", 1, 1, 0, 8,
                   125000, 0, out1, 2, 1);
    }
    printf("faults %d\n", g_faults);
    return g_faults != 0;
}
