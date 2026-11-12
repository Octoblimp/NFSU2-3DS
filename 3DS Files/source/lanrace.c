// lanrace -- a LAN race's cars, start and states: see lanrace.h.

#include "lanrace.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PIECE (WL_MSG_MAX - 4)   // a piece: its offset (2), the total (2)

static void say(LanRace *R, const char *fmt, ...)
{
    char    line[192];
    va_list ap;

    if (!R->log) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    R->log(line);
}

static void put16(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static uint32_t get16(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8;
}

static uint32_t get32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}

static void putf(uint8_t *b, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(b, v);
}

static float getf(const uint8_t *b)
{
    uint32_t v = get32(b);
    float    f;
    memcpy(&f, &v, 4);
    return f;
}

static int bits(uint8_t v)
{
    int n = 0;
    for (; v; v &= (uint8_t)(v - 1)) n++;
    return n;
}

static void push_event(LanRace *R, LanRaceEvKind kind, int console)
{
    LanRaceEvent *e;

    if (R->evCount == LANRACE_EVENTS) {       // the oldest goes
        R->evHead = (R->evHead + 1) % LANRACE_EVENTS;
        R->evCount--;
    }
    e = &R->ev[(R->evHead + R->evCount) % LANRACE_EVENTS];
    e->kind    = kind;
    e->console = (uint8_t)console;
    R->evCount++;
}

// ------------------------------------------------------------ the wire

void lanrace_encode_car(const LanRaceCar *c, uint8_t *b, uint16_t *n)
{
    int      k, o = 17, count = 0;

    put32(b, c->carType);
    put32(b + 4, c->nameHash);
    putf(b + 8, c->camber);
    b[12] = c->sndEng;
    b[13] = c->sndTrans;
    b[14] = c->sndTurbo;
    b[15] = c->nosUpgrade;
    for (k = 0; k < LANRACE_RIDE_SLOTS; k++) {
        if (!c->ride[k]) continue;
        b[o] = (uint8_t)k;
        put32(b + o + 1, c->ride[k]);
        o += 5;
        count++;
    }
    b[16] = (uint8_t)count;
    *n = (uint16_t)o;
}

int lanrace_decode_car(LanRaceCar *c, const uint8_t *b, uint16_t n)
{
    int k, count;

    if (n < 17) return 0;
    count = b[16];
    if (n != 17 + 5 * count) return 0;
    memset(c, 0, sizeof *c);
    c->carType    = get32(b);
    c->nameHash   = get32(b + 4);
    c->camber     = getf(b + 8);
    c->sndEng     = b[12];
    c->sndTrans   = b[13];
    c->sndTurbo   = b[14];
    c->nosUpgrade = b[15];
    for (k = 0; k < count; k++) {
        const uint8_t *p = b + 17 + 5 * k;
        if (p[0] >= LANRACE_RIDE_SLOTS) return 0;
        c->ride[p[0]] = get32(p + 1);
    }
    return 1;
}

void lanrace_encode_state(const LanRaceState *s, int console, uint8_t *b)
{
    int k;

    b[0] = (uint8_t)console;
    put16(b + 1, s->seq);
    put32(b + 3, s->ms);
    for (k = 0; k < 3; k++) {
        putf(b + 7 + 4 * k, s->pos[k]);
        putf(b + 19 + 4 * k, s->fwd[k]);
        putf(b + 31 + 4 * k, s->up[k]);
        putf(b + 43 + 4 * k, s->vel[k]);
        putf(b + 55 + 4 * k, s->w[k]);
    }
    put16(b + 67, s->steerU16);
    b[69] = s->thr;
    b[70] = s->brake;
    b[71] = s->flags;
    b[72] = (uint8_t)s->gear;
    put16(b + 73, s->rpm);
    b[75] = s->lap;
}

int lanrace_decode_state(LanRaceState *s, int *console, const uint8_t *b,
                         size_t n)
{
    int k;

    if (n < LANRACE_STATE_WIRE || b[0] >= LAN_CONSOLES) return 0;
    memset(s, 0, sizeof *s);
    *console = b[0];
    s->seq = (uint16_t)get16(b + 1);
    s->ms  = get32(b + 3);
    for (k = 0; k < 3; k++) {
        s->pos[k] = getf(b + 7 + 4 * k);
        s->fwd[k] = getf(b + 19 + 4 * k);
        s->up[k]  = getf(b + 31 + 4 * k);
        s->vel[k] = getf(b + 43 + 4 * k);
        s->w[k]   = getf(b + 55 + 4 * k);
    }
    s->steerU16 = (uint16_t)get16(b + 67);
    s->thr      = b[69];
    s->brake    = b[70];
    s->flags    = b[71];
    s->gear     = (int8_t)b[72];
    s->rpm      = (uint16_t)get16(b + 73);
    s->lap      = b[75];
    return 1;
}

// ------------------------------------------------------------ sending

static int send(LanRace *R, int console, uint8_t type, const void *d,
                size_t n, int reliable)
{
    if (console == R->self) return 1;
    return R->net->send(R->net->ctx, console, type, d, n, reliable);
}

// A reliable message in pieces, from the next frame on; `d` stays put.
static void queue_out(LanRace *R, int console, uint8_t type,
                      const uint8_t *d, uint16_t n)
{
    LanRaceOut *o = &R->out[console];

    o->type = type;
    o->d    = d;
    o->n    = n;
    o->off  = 0;
}

static void pump_out(LanRace *R)
{
    uint8_t b[WL_MSG_MAX];
    int     c;

    for (c = 0; c < LAN_CONSOLES; c++) {
        LanRaceOut *o = &R->out[c];
        while (o->d && o->off < o->n) {
            uint16_t k = (uint16_t)(o->n - o->off);
            if (k > PIECE) k = PIECE;
            put16(b, o->off);
            put16(b + 2, o->n);
            memcpy(b + 4, o->d + o->off, k);
            if (!send(R, c, o->type, b, (size_t)(4 + k), 1)) break;
            o->off = (uint16_t)(o->off + k);
        }
        if (o->d && o->off >= o->n) o->d = NULL;
    }
}

// A piece into `buf`: 1 once the whole has come. Pieces come in order on
// the reliable link, so `*got` is where the next one starts.
static int take_piece(uint8_t *buf, size_t cap, uint16_t *len, uint16_t *got,
                      const uint8_t *d, size_t n)
{
    uint16_t off, total, k;

    if (n < 4) return 0;
    off   = (uint16_t)get16(d);
    total = (uint16_t)get16(d + 2);
    k     = (uint16_t)(n - 4);
    if (total > cap || off != *got || off + k > total) return 0;
    if (off == 0) *len = total;
    if (total != *len) return 0;
    memcpy(buf + off, d + 4, k);
    *got = (uint16_t)(off + k);
    return *got == *len;
}

// ------------------------------------------------------------ an Outrun

// The PC's LAN / online Outrun (#488) is no track race: race type 7 on an
// Outrun spawn (tracks 4091..4094, which have a grid and no finish line), and
// the server's UpdateOutrun (SPEED2.EXE 0x5F9CD0, from 0x60AEE0 while the race
// type is 7) plays a lead game over the racers' cars:
//   * no leader, or the leader gone: the first racer still in takes the lead
//     (grid order);
//   * a racer more than GetGetAwayDistance (0x5EEEA0: 300 m, or 300 + 4.9 --
//     1000 ft -- off the metric option) from the leader is out (message 0x10,
//     its time);
//   * one within 30 m (0x7A56B0) that is ahead of the leader and facing its
//     way, at 10 m/s (0x7A56A8) or more or with the leader under that, takes
//     the lead -- the one furthest ahead, and not within 8000 ticks (2 s) of
//     the last change (message 0xF);
//   * none left within the distance: the leader wins (0x5F9C80);
//   * the room's time limit (0x7A5474: 0, 2, 5, 8, 10 minutes) up: everyone
//     else is out, the leader wins.
// Points to Win is the room's to show (0x4B3640, 0x4D27FF); no race code
// reads it.
// PORT: "ahead" is along the leader's heading (its velocity, or its nose
// under 10 m/s), as retail's slow-leader arm has it; retail's fast arm reads
// the cars' road positions, which the server here has no road for. The time
// limit runs from START, the countdown in it.
static const uint8_t kOtrMinutes[5] = { 0, 2, 5, 8, 10 };
#define OTR_PASS_M     30.0f
#define OTR_PASS_MS    10.0f
#define OTR_COOL_MS    2000u

static int is_racer(const LanRace *R, int c);

// The lead and the outs each racer has not been told yet, again each frame
// until its link takes them. The outs go in the order they fell, the win
// only after every one of them: a racer places each car out in the lowest
// place still free (race_retire), so the order is the result.
static void otr_tell(LanRace *R)
{
    uint8_t b[6];
    int     c, k;

    for (c = 0; c < LAN_CONSOLES; c++) {
        if (c == R->self || !is_racer(R, c)) continue;
        for (;;) {
            const uint8_t left = (uint8_t)(R->otrOut & ~R->otrOutTold[c]);
            int pick = -1;
            for (k = 0; k < LAN_CONSOLES; k++) {
                if (!((left >> k) & 1) || R->otrWon == k) continue;
                if (pick < 0 || R->otrOutMs[k] < R->otrOutMs[pick]) pick = k;
            }
            if (pick < 0 && R->otrWon < LAN_CONSOLES && ((left >> R->otrWon) & 1))
                pick = R->otrWon;
            if (pick < 0) break;
            b[0] = (uint8_t)pick;
            put32(b + 1, R->otrOutMs[pick]);
            b[5] = (uint8_t)(R->otrWon == pick);
            if (!send(R, c, LANRACE_MSG_OUT, b, 6, 1)) break;
            R->otrOutTold[c] |= (uint8_t)(1u << pick);
        }
        if (R->leader >= 0 && R->otrLeadTold[c] != (uint8_t)(R->leader + 1)) {
            b[0] = (uint8_t)R->leader;
            put32(b + 1, R->now - R->otrStart);
            if (send(R, c, LANRACE_MSG_LEAD, b, 5, 1))
                R->otrLeadTold[c] = (uint8_t)(R->leader + 1);
        }
    }
}

static void otr_lead(LanRace *R, int c, uint32_t t)
{
    R->leader   = (int8_t)c;
    R->leadCool = R->now + OTR_COOL_MS;
    say(R, "lanrace: outrun -- console %d has the lead at %lu ms", c,
        (unsigned long)t);
    if (R->racing) push_event(R, LANRACE_EV_LEAD, c);
}

static void otr_out(LanRace *R, int c, uint32_t t, int won)
{
    R->otrOut |= (uint8_t)(1u << c);
    R->otrOutMs[c] = t;
    if (won) R->otrWon = (uint8_t)c;
    say(R, "lanrace: outrun -- console %d %s at %lu ms", c,
        won ? "wins" : "is out", (unsigned long)t);
    if (R->racing) push_event(R, LANRACE_EV_OUT, c);
}

static void outrun_frame(LanRace *R)
{
    const uint32_t t    = R->now - R->otrStart;
    const uint8_t  live = (uint8_t)(R->in & ~R->otrOut);
    const LanRaceState *ls;
    float lspd2, dx, dy, dl;
    int   i, c, L, near = 0, best = -1;
    float bestM = 0.0f;

    if (!live || R->otrWon != 0xff) return;
    if (R->leader < 0 || !((live >> R->leader) & 1)) {
        for (i = 0; i < R->racers && i < LAN_CONSOLES; i++) {
            c = R->console[i];
            if (c < LAN_CONSOLES && ((live >> c) & 1)) {
                otr_lead(R, c, t);
                break;
            }
        }
        return;
    }
    L = R->leader;
    if (R->otrLimitMs && t >= R->otrLimitMs) {
        for (c = 0; c < LAN_CONSOLES; c++)
            if (c != L && ((live >> c) & 1)) otr_out(R, c, t, 0);
        otr_out(R, L, t, 1);
        return;
    }
    if (!((R->stIn >> L) & 1)) return;
    ls    = &R->st[L];
    lspd2 = ls->vel[0] * ls->vel[0] + ls->vel[1] * ls->vel[1] +
            ls->vel[2] * ls->vel[2];
    if (lspd2 >= OTR_PASS_MS * OTR_PASS_MS) {
        dx = ls->vel[0];
        dy = ls->vel[1];
    } else {
        dx = ls->fwd[0];
        dy = ls->fwd[1];
    }
    dl = dx * dx + dy * dy;
    for (c = 0; c < LAN_CONSOLES; c++) {
        const LanRaceState *s = &R->st[c];
        float rx, ry, rz, d2, spd2, ahead;

        if (c == L || !((live >> c) & 1)) continue;
        if (!((R->stIn >> c) & 1)) { near++; continue; }
        rx = s->pos[0] - ls->pos[0];
        ry = s->pos[1] - ls->pos[1];
        rz = s->pos[2] - ls->pos[2];
        d2 = rx * rx + ry * ry + rz * rz;
        if (d2 > R->otrDist * R->otrDist) {
            otr_out(R, c, t, 0);
            continue;
        }
        near++;
        spd2 = s->vel[0] * s->vel[0] + s->vel[1] * s->vel[1] +
               s->vel[2] * s->vel[2];
        if (d2 > OTR_PASS_M * OTR_PASS_M || dl <= 0.0f ||
            !(spd2 >= OTR_PASS_MS * OTR_PASS_MS ||
              lspd2 < OTR_PASS_MS * OTR_PASS_MS))
            continue;
        ahead = rx * dx + ry * dy;
        if (ahead < 0.0f || s->fwd[0] * dx + s->fwd[1] * dy < 0.0f) continue;
        if (best < 0 || ahead > bestM) {
            best  = c;
            bestM = ahead;
        }
    }
    if (!near) {
        otr_out(R, L, t, 1);
        return;
    }
    if (best >= 0 && (int32_t)(R->now - R->leadCool) >= 0)
        otr_lead(R, best, t);
}

// ------------------------------------------------------------ the server

static int is_racer(const LanRace *R, int c)
{
    return c >= 0 && c < LAN_CONSOLES && ((R->in >> c) & 1);
}

static void relay(LanRace *R, int from, uint8_t type, const uint8_t *d,
                  size_t n, int reliable)
{
    int c;

    for (c = 0; c < LAN_CONSOLES; c++)
        if (c != from && c != R->self && is_racer(R, c))
            send(R, c, type, d, n, reliable);
}

static void abort_race(LanRace *R, const char *why)
{
    say(R, "lanrace: no race -- %s", why);
    if (R->racing) push_event(R, LANRACE_EV_ABORT, R->self);
    R->stage = LANRACE_OFF;
}

// The server's: racer `c` out of the race, the others told.
static void drop(LanRace *R, int c, const char *why)
{
    uint8_t b = (uint8_t)c;

    if (!is_racer(R, c)) return;
    R->in &= (uint8_t)~(1u << c);
    say(R, "lanrace: console %d leaves the race (%s), %d still in", c, why,
        bits(R->in));
    if (c != R->self) send(R, c, LANRACE_MSG_LEFT, &b, 1, 1);  // to itself too
    relay(R, c, LANRACE_MSG_LEFT, &b, 1, 1);
    if (R->racing && c != R->self) push_event(R, LANRACE_EV_LEFT, c);
    R->out[c].d = NULL;
    if (R->stage < LANRACE_RACING && bits(R->in) < 2) {
        uint8_t k;
        for (k = 0; k < LAN_CONSOLES; k++)       // the last one out too
            if (is_racer(R, k) && k != R->self)
                send(R, k, LANRACE_MSG_LEFT, &k, 1, 1);
        R->in = 0;
        abort_race(R, "fewer than two racers left");
    }
}

static void build_table(LanRace *R)
{
    int i, o = 1 + LAN_CONSOLES;

    R->table[0] = (uint8_t)R->racers;
    memset(R->table + 1, 0xff, LAN_CONSOLES);
    for (i = 0; i < R->racers; i++) {
        const int c = R->console[i];
        if (!is_racer(R, c)) continue;        // left: not in the table
        R->table[1 + i] = (uint8_t)c;
        put16(R->table + o, R->carWireLen[c]);
        memcpy(R->table + o + 2, R->carWire[c], R->carWireLen[c]);
        o += 2 + R->carWireLen[c];
    }
    R->tableLen = (uint16_t)o;
}

static void server_frame(LanRace *R)
{
    int c;

    for (c = 0; c < LAN_CONSOLES; c++)
        if (c != R->self && is_racer(R, c) && !R->net->alive(R->net->ctx, c))
            drop(R, c, "its link lost");
    if (R->stage == LANRACE_OFF) return;

    if (R->stage == LANRACE_CARS) {
        if ((R->carIn & R->in) != R->in) {
            if (R->now - R->since < LANRACE_PATIENCE_MS) return;
            for (c = 0; c < LAN_CONSOLES; c++)
                if (is_racer(R, c) && !((R->carIn >> c) & 1))
                    drop(R, c, "no car in time");
            if (R->stage == LANRACE_OFF) return;
        }
        build_table(R);
        for (c = 0; c < LAN_CONSOLES; c++)
            if (c != R->self && is_racer(R, c))
                queue_out(R, c, LANRACE_MSG_CARS, R->table, R->tableLen);
        say(R, "lanrace: every car in (%d racers), the table out, %u bytes",
            bits(R->in), (unsigned)R->tableLen);
        R->stage = R->racing ? LANRACE_LOADING : LANRACE_HOLD;
        R->since = R->now;
        if (R->racing) push_event(R, LANRACE_EV_CARS, R->self);
        return;
    }
    if (R->stage == LANRACE_LOADING || R->stage == LANRACE_HOLD) {
        if ((R->loaded & R->in) != R->in) {
            if (R->now - R->since < LANRACE_PATIENCE_MS) return;
            for (c = 0; c < LAN_CONSOLES; c++)
                if (is_racer(R, c) && !((R->loaded >> c) & 1))
                    drop(R, c, "not loaded in time");
            if (R->stage == LANRACE_OFF) return;
        }
        if (R->stage == LANRACE_LOADING) return;   // its own still loading
        for (c = 0; c < LAN_CONSOLES; c++)
            if (c != R->self && is_racer(R, c))
                send(R, c, LANRACE_MSG_START, "", 1, 1);
        say(R, "lanrace: every racer loaded -- START, %d racers", bits(R->in));
        R->stage = LANRACE_RACING;
        R->since = R->now;
        R->otrStart = R->now;
        if (R->racing) push_event(R, LANRACE_EV_START, R->self);
        return;
    }
    if (R->stage == LANRACE_RACING && R->outrun) {
        outrun_frame(R);
        otr_tell(R);
    }
}

static void server_message(LanRace *R, int c, uint8_t type,
                           const uint8_t *d, size_t n)
{
    LanRaceState s;
    int          who;

    if (!is_racer(R, c)) return;
    switch (type) {
    case LANRACE_MSG_CAR:
        if (R->stage != LANRACE_CARS || ((R->carIn >> c) & 1)) break;
        if (take_piece(R->carWire[c], LANRACE_CAR_WIRE_MAX, &R->carWireLen[c],
                       &R->carWireGot[c], d, n)) {
            if (lanrace_decode_car(&R->car[c], R->carWire[c],
                                   R->carWireLen[c])) {
                R->carIn |= (uint8_t)(1u << c);
                say(R, "lanrace: console %d's car in, type %lu", c,
                    (unsigned long)R->car[c].carType);
            } else {
                drop(R, c, "its car unreadable");
            }
        }
        break;
    case LANRACE_MSG_LOADED:
        if (R->stage == LANRACE_LOADING || R->stage == LANRACE_HOLD) {
            R->loaded |= (uint8_t)(1u << c);
            say(R, "lanrace: console %d loaded", c);
        }
        break;
    case LANRACE_MSG_STATE:
        if (R->stage != LANRACE_RACING || !lanrace_decode_state(&s, &who, d, n)
            || who != c)
            break;
        R->st[c]   = s;
        R->stIn   |= (uint8_t)(1u << c);
        R->stAt[c] = R->now;
        relay(R, c, LANRACE_MSG_STATE, d, n, 0);
        break;
    case LANRACE_MSG_FINISH:
        if (n < 5 || d[0] != c || ((R->finished >> c) & 1)) break;
        R->finishMs[c] = get32(d + 1);
        R->finished   |= (uint8_t)(1u << c);
        relay(R, c, LANRACE_MSG_FINISH, d, 5, 1);
        say(R, "lanrace: console %d finished at %lu ms", c,
            (unsigned long)R->finishMs[c]);
        if (R->racing) push_event(R, LANRACE_EV_FINISH, c);
        break;
    case LANRACE_MSG_LEFT:
        drop(R, c, "it left");
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------ a racer

static void take_table(LanRace *R)
{
    int i, o = 1 + LAN_CONSOLES, n = R->table[0];

    if (n < 2 || n > LAN_CONSOLES || R->tableLen < o) goto bad;
    R->in      = 0;
    R->carIn   = 0;
    R->racers  = n;
    for (i = 0; i < n; i++) {
        const uint8_t c = R->table[1 + i];
        R->console[i] = c;
        if (c == 0xff) continue;               // left before the table
        if (c >= LAN_CONSOLES || o + 2 > R->tableLen) goto bad;
        {
            const uint16_t len = (uint16_t)get16(R->table + o);
            if (o + 2 + len > R->tableLen ||
                !lanrace_decode_car(&R->car[c], R->table + o + 2, len))
                goto bad;
            o += 2 + len;
        }
        R->in    |= (uint8_t)(1u << c);
        R->carIn |= (uint8_t)(1u << c);
    }
    if (!is_racer(R, R->self)) goto bad;
    say(R, "lanrace: the table in, %d racers", bits(R->in));
    R->stage = LANRACE_LOADING;
    R->since = R->now;
    push_event(R, LANRACE_EV_CARS, R->self);
    return;
bad:
    abort_race(R, "the cars' table unreadable");
}

static void racer_message(LanRace *R, int from, uint8_t type,
                          const uint8_t *d, size_t n)
{
    LanRaceState s;
    int          who;

    if (from != 0) return;                     // the server's alone
    switch (type) {
    case LANRACE_MSG_CARS:
        if (R->stage != LANRACE_CARS) break;
        if (take_piece(R->table, sizeof R->table, &R->tableLen, &R->tableGot,
                       d, n))
            take_table(R);
        break;
    case LANRACE_MSG_START:
        if (R->stage != LANRACE_HOLD && R->stage != LANRACE_LOADING) break;
        say(R, "lanrace: START");
        R->stage = LANRACE_RACING;
        R->since = R->now;
        push_event(R, LANRACE_EV_START, R->self);
        break;
    case LANRACE_MSG_STATE:
        if (R->stage != LANRACE_RACING || !lanrace_decode_state(&s, &who, d, n)
            || who == R->self || !is_racer(R, who))
            break;
        R->st[who]   = s;
        R->stIn     |= (uint8_t)(1u << who);
        R->stAt[who] = R->now;
        break;
    case LANRACE_MSG_FINISH:
        if (n < 5 || d[0] >= LAN_CONSOLES || ((R->finished >> d[0]) & 1))
            break;
        R->finishMs[d[0]] = get32(d + 1);
        R->finished      |= (uint8_t)(1u << d[0]);
        push_event(R, LANRACE_EV_FINISH, d[0]);
        break;
    case LANRACE_MSG_LEAD:
        if (!R->outrun || n < 5 || d[0] >= LAN_CONSOLES) break;
        R->leader = (int8_t)d[0];
        say(R, "lanrace: outrun -- console %d has the lead", d[0]);
        push_event(R, LANRACE_EV_LEAD, d[0]);
        break;
    case LANRACE_MSG_OUT:
        if (!R->outrun || n < 6 || d[0] >= LAN_CONSOLES ||
            ((R->otrOut >> d[0]) & 1))
            break;
        R->otrOut |= (uint8_t)(1u << d[0]);
        R->otrOutMs[d[0]] = get32(d + 1);
        if (d[5]) R->otrWon = d[0];
        say(R, "lanrace: outrun -- console %d %s", d[0], d[5] ? "wins" : "is out");
        push_event(R, LANRACE_EV_OUT, d[0]);
        break;
    case LANRACE_MSG_LEFT:
        if (n < 1 || d[0] >= LAN_CONSOLES || R->stage == LANRACE_OFF) break;
        if (d[0] == R->self) {
            abort_race(R, "the server took this console out");
            break;
        }
        if (!is_racer(R, d[0])) break;
        R->in &= (uint8_t)~(1u << d[0]);
        say(R, "lanrace: console %d left the race", d[0]);
        push_event(R, LANRACE_EV_LEFT, d[0]);
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------ the calls

void lanrace_init(LanRace *R, const LanRaceNet *net, LanLogFn log)
{
    memset(R, 0, sizeof *R);
    R->net = net;
    R->log = log;
}

void lanrace_begin(LanRace *R, const LanGo *go, int self, int server,
                   const LanRaceCar *car, uint32_t nowMs)
{
    const LanRaceNet *net = R->net;
    LanLogFn          log = R->log;
    int               i;

    memset(R, 0, sizeof *R);
    R->net    = net;
    R->log    = log;
    R->self   = self;
    R->server = server;
    R->now    = R->since = nowMs;
    R->racers = go->racers;
    memset(R->console, 0xff, sizeof R->console);
    for (i = 0; i < go->racers && i < LAN_CONSOLES; i++) {
        const int c = go->console[i];
        R->console[i] = (uint8_t)c;
        if (c >= 0 && c < LAN_CONSOLES) R->in |= (uint8_t)(1u << c);
        if (c == self) R->racing = 1;
    }
    R->stage = LANRACE_CARS;
    R->leader = -1;
    R->otrWon = 0xff;
    R->otrDist = 304.9f;
    if (go->set.mode == 8) {
        R->outrun = 1;
        R->otrLimitMs = (uint32_t)kOtrMinutes[go->set.outrunTime < 5 ?
                                              go->set.outrunTime : 0] * 60000u;
    }
    if (R->racing && car) {
        R->car[self] = *car;
        lanrace_encode_car(car, R->carWire[self], &R->carWireLen[self]);
        R->carWireGot[self] = R->carWireLen[self];
        R->carIn |= (uint8_t)(1u << self);
        if (!server)
            queue_out(R, 0, LANRACE_MSG_CAR, R->carWire[self],
                      R->carWireLen[self]);
    } else if (R->racing) {
        R->racing = 0;                         // no car: out at once
        R->in &= (uint8_t)~(1u << self);
    }
    say(R, "lanrace: begin, console %d%s, %d racers%s", self,
        server ? " (the server)" : "", go->racers,
        R->racing ? "" : ", not racing");
    pump_out(R);
}

void lanrace_message(LanRace *R, int console, uint8_t type,
                     const uint8_t *d, size_t n)
{
    if (R->stage == LANRACE_OFF || console < 0 || console >= LAN_CONSOLES)
        return;
    if (R->server) server_message(R, console, type, d, n);
    else           racer_message(R, console, type, d, n);
}

void lanrace_frame(LanRace *R, uint32_t nowMs)
{
    R->now = nowMs;
    if (R->stage == LANRACE_OFF) return;
    if (R->server) {
        server_frame(R);
    } else if (!R->net->alive(R->net->ctx, 0)) {
        abort_race(R, "the server's link lost");
        return;
    }
    pump_out(R);
}

void lanrace_loaded(LanRace *R)
{
    if (R->stage != LANRACE_LOADING) return;
    R->stage = LANRACE_HOLD;
    if (R->server) {
        R->loaded |= (uint8_t)(1u << R->self);
        say(R, "lanrace: loaded (the server)");
        server_frame(R);
    } else {
        send(R, 0, LANRACE_MSG_LOADED, "", 1, 1);
        say(R, "lanrace: loaded, START waited for");
    }
}

void lanrace_put_state(LanRace *R, const LanRaceState *s)
{
    uint8_t b[LANRACE_STATE_WIRE];

    if (R->stage != LANRACE_RACING || !R->racing) return;
    lanrace_encode_state(s, R->self, b);
    R->st[R->self] = *s;
    R->stIn |= (uint8_t)(1u << R->self);   // an Outrun's server reads its own
    if (R->server) relay(R, R->self, LANRACE_MSG_STATE, b, sizeof b, 0);
    else           send(R, 0, LANRACE_MSG_STATE, b, sizeof b, 0);
}

void lanrace_finished(LanRace *R, uint32_t ms)
{
    uint8_t b[5];

    if (R->stage != LANRACE_RACING || !R->racing ||
        ((R->finished >> R->self) & 1))
        return;
    R->finishMs[R->self] = ms;
    R->finished |= (uint8_t)(1u << R->self);
    b[0] = (uint8_t)R->self;
    put32(b + 1, ms);
    if (R->server) relay(R, R->self, LANRACE_MSG_FINISH, b, 5, 1);
    else           send(R, 0, LANRACE_MSG_FINISH, b, 5, 1);
    say(R, "lanrace: finished at %lu ms", (unsigned long)ms);
}

void lanrace_leave(LanRace *R)
{
    uint8_t b = (uint8_t)R->self;

    if (R->stage == LANRACE_OFF) return;
    if (R->racing) {
        if (R->server) {
            drop(R, R->self, "it left");
            R->racing = 0;
            if (R->stage != LANRACE_OFF && bits(R->in) == 0)
                R->stage = LANRACE_OFF;
            return;                            // it still relays the rest
        }
        send(R, 0, LANRACE_MSG_LEFT, &b, 1, 1);
        say(R, "lanrace: left the race");
    }
    R->racing = 0;
    R->stage  = LANRACE_OFF;
}

void lanrace_outrun_distance(LanRace *R, float m)
{
    if (m > 0.0f) R->otrDist = m;
}

int lanrace_event(LanRace *R, LanRaceEvent *out)
{
    if (R->evCount == 0) return 0;
    *out = R->ev[R->evHead];
    R->evHead = (R->evHead + 1) % LANRACE_EVENTS;
    R->evCount--;
    return 1;
}

int lanrace_grid(const LanRace *R, int console)
{
    int i;

    for (i = 0; i < R->racers && i < LAN_CONSOLES; i++)
        if (R->console[i] == console) return i;
    return -1;
}
