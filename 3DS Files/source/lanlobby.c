// lanlobby -- the LAN lobby over local wireless (TASKS #321b). See
// lanlobby.h for the design and docs/formats/lan-lobby-pc-re.md for the
// PC's flow it follows.
#include "lanlobby.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define APPDATA_TAG     'L'
#define APPDATA_VERSION 1
#define APPDATA_SIZE    (2 + LAN_NAME_MAX)   // tag, version, the server's name

#define NAMES_SIZE (1 + LAN_CONSOLES * LAN_NAME_MAX)
#define GAME_WIRE  (6 + LAN_SETTINGS_WIRE)
#define GO_SIZE    (1 + 4 + 1 + LAN_CONSOLES + LAN_SETTINGS_WIRE)

static void logf_(LanLobby *L, const char *fmt, ...)
{
    char    b[160];
    va_list ap;

    if (!L->log)
        return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    L->log(b);
}

static void copy_name(char *dst, const char *src)
{
    size_t n = src ? strlen(src) : 0;

    if (n > LAN_NAME_TYPED)
        n = LAN_NAME_TYPED;
    memset(dst, 0, LAN_NAME_MAX);
    if (n)
        memcpy(dst, src, n);
}

static void copy_password(char *dst, const char *src)
{
    size_t n = src ? strlen(src) : 0;

    if (n > LAN_PASSWORD_MAX - 1)
        n = LAN_PASSWORD_MAX - 1;
    memset(dst, 0, LAN_PASSWORD_MAX);
    if (n)
        memcpy(dst, src, n);
}

static void push_event(LanLobby *L, LanEventKind kind, uint8_t game, LanResult r)
{
    LanEvent *e;

    if (L->evCount == LAN_EVENTS) {          // the oldest goes
        L->evHead = (L->evHead + 1) % LAN_EVENTS;
        L->evCount--;
    }
    e = &L->ev[(L->evHead + L->evCount) % LAN_EVENTS];
    e->kind   = kind;
    e->game   = game;
    e->result = r;
    L->evCount++;
}

int lanlobby_event(LanLobby *L, LanEvent *out)
{
    if (L->evCount == 0)
        return 0;
    *out = L->ev[L->evHead];
    L->evHead = (L->evHead + 1) % LAN_EVENTS;
    L->evCount--;
    return 1;
}

// ------------------------------------------------------------ settings

static void put_settings(uint8_t *p, const LanSettings *s)
{
    p[0]  = s->maxPlayers;
    p[1]  = s->mode;
    p[2]  = (uint8_t)(s->track & 0xff);
    p[3]  = (uint8_t)(s->track >> 8);
    p[4]  = s->reverse;
    p[5]  = s->laps;
    p[6]  = s->restriction;
    p[7]  = s->raceBreaker;
    p[8]  = s->collision;
    p[9]  = s->outrunPoints;
    p[10] = s->outrunTime;
    p[11] = s->locked;
    p[12] = 0;
}

static void get_settings(LanSettings *s, const uint8_t *p)
{
    s->maxPlayers   = p[0];
    s->mode         = p[1];
    s->track        = (uint16_t)(p[2] | p[3] << 8);
    s->reverse      = p[4];
    s->laps         = p[5];
    s->restriction  = p[6];
    s->raceBreaker  = p[7];
    s->collision    = p[8];
    s->outrunPoints = p[9];
    s->outrunTime   = p[10];
    s->locked       = p[11];
}

void lanlobby_lap_range(uint8_t mode, int pointToPoint, int *lo, int *hi)
{
    // 0x4B3E40 / 0x4B3F10: no laps row for Sprint, Drag, Outrun or a
    // point-to-point Drift, which race one lap.
    *lo = *hi = 1;
    if (mode == 0 || mode == 4) {               // Circuit, URL
        *lo = 2;
        *hi = 10;
    } else if ((mode == 3 && !pointToPoint) || mode == 5 || mode == 10) {
        *lo = 4;                                // Drift, Street X
        *hi = 10;
    }
}

void lanlobby_default_settings(LanSettings *s, uint8_t mode, uint16_t track,
                               int pointToPoint)
{
    int lo, hi;

    memset(s, 0, sizeof *s);
    s->maxPlayers = LAN_CONSOLES;
    s->mode       = mode;
    s->track      = track;
    lanlobby_lap_range(mode, pointToPoint, &lo, &hi);
    s->laps       = (uint8_t)lo;
    s->collision  = 1;
}

// Clamped as UI_QRModeOptions' rows would leave them: a client's settings
// are the server's to trust no further than that.
static void clamp_settings(LanSettings *s)
{
    int lo, hi;

    if (s->maxPlayers < 2)
        s->maxPlayers = 2;
    if (s->maxPlayers > LAN_CONSOLES)
        s->maxPlayers = LAN_CONSOLES;
    // The server looks no track up: a Drift sent with one lap is taken
    // for a point-to-point one, as the creator's screen left it.
    lanlobby_lap_range(s->mode, s->laps == 1, &lo, &hi);
    if (s->laps < lo)
        s->laps = (uint8_t)lo;
    if (s->laps > hi)
        s->laps = (uint8_t)hi;
    if (s->mode == 3 || s->mode == 10)      // FUN_004b41c0: no Race Breaker
        s->raceBreaker = 0;
    if (s->mode == 8)                       // Outrun: no direction
        s->reverse = 0;
    s->reverse     = s->reverse ? 1 : 0;
    s->collision   = s->collision ? 1 : 0;
    s->restriction = s->restriction ? 1 : 0;
    s->raceBreaker = s->raceBreaker ? 1 : 0;
}

// ------------------------------------------------------------ the games

static LanGame *find_game(LanLobby *L, uint8_t id)
{
    int i;

    if (id == 0)
        return NULL;
    for (i = 0; i < L->numGames; i++)
        if (L->games[i].id == id)
            return &L->games[i];
    return NULL;
}

const LanGame *lanlobby_game(const LanLobby *L, uint8_t id)
{
    return find_game((LanLobby *)L, id);
}

const LanGo *lanlobby_go(const LanLobby *L)
{
    return &L->go;
}

static int count_bits(uint8_t m)
{
    int n = 0;

    for (; m; m &= (uint8_t)(m - 1))
        n++;
    return n;
}

static LanGame *game_of(LanLobby *L, int console)
{
    int i;

    for (i = 0; i < L->numGames; i++)
        if (L->games[i].members & (1u << console))
            return &L->games[i];
    return NULL;
}

static void remove_game(LanLobby *L, LanGame *g)
{
    int k = (int)(g - L->games);

    for (; k + 1 < L->numGames; k++) {
        L->games[k] = L->games[k + 1];
        memcpy(L->password[k], L->password[k + 1], LAN_PASSWORD_MAX);
        L->countdownEnd[k] = L->countdownEnd[k + 1];
    }
    L->numGames--;
    memset(&L->games[L->numGames], 0, sizeof L->games[0]);
    memset(L->password[L->numGames], 0, LAN_PASSWORD_MAX);
    L->countdownEnd[L->numGames] = 0;
}

// ------------------------------------------------------------ sending

static void send_to(LanLobby *L, int console, uint8_t type, const void *d,
                    size_t n)
{
    // #321b: never a message of no bytes -- a client's zero-length LEAVE
    // broke the link on Azahar (the host's every udsPullPacket after it
    // "too large", rebinding no help) until both sides timed out. One byte,
    // which no handler of those messages reads.
    static const uint8_t none = 0;
    int peer = L->server ? console - 1 : 0;

    if (n == 0) {
        d = &none;
        n = 1;
    }
    if (peer < 0 || !L->net->send(L->net->ctx, peer, type, d, n))
        logf_(L, "lan: message %02x to %d not sent", type, console);
}

static void apply_result(LanLobby *L, uint8_t op, LanResult code, uint8_t game);

// A request's answer: the server's own straight to its events.
static void reply(LanLobby *L, int console, uint8_t op, LanResult code,
                  uint8_t game)
{
    uint8_t b[3];

    if (console == 0) {
        apply_result(L, op, code, game);
        return;
    }
    b[0] = op;
    b[1] = (uint8_t)code;
    b[2] = game;
    send_to(L, console, LAN_MSG_RESULT, b, sizeof b);
}

static size_t pack_games(const LanLobby *L, uint8_t *b)
{
    int     i;
    uint8_t *p = b + 1;

    b[0] = (uint8_t)L->numGames;
    for (i = 0; i < L->numGames; i++, p += GAME_WIRE) {
        const LanGame *g = &L->games[i];
        p[0] = g->id;
        p[1] = g->creator;
        p[2] = g->members;
        p[3] = g->ready;
        p[4] = g->started;
        p[5] = g->countdown;
        put_settings(p + 6, &g->set);
    }
    return (size_t)(p - b);
}

static void broadcast_lists(LanLobby *L)
{
    uint8_t names[NAMES_SIZE], games[1 + LAN_MAX_GAMES * GAME_WIRE];
    size_t  ng;
    int     c;

    names[0] = L->present;
    memcpy(names + 1, L->name, LAN_CONSOLES * LAN_NAME_MAX);
    ng = pack_games(L, games);
    for (c = 1; c < LAN_CONSOLES; c++)
        if (L->present & (1u << c)) {
            send_to(L, c, LAN_MSG_NAMES, names, sizeof names);
            send_to(L, c, LAN_MSG_GAMES, games, ng);
        }
    push_event(L, LAN_EV_LISTS, 0, LAN_OK);
    L->dirty = 0;
}

// ------------------------------------------------------------ the server

// Out of its game: a creator's game closes and its racers are told.
static void server_leave_game(LanLobby *L, int console, LanResult why)
{
    LanGame *g = game_of(L, console);
    int      c;

    if (!g)
        return;
    if (g->creator == console) {
        uint8_t id = g->id;
        for (c = 0; c < LAN_CONSOLES; c++)
            if (c != console && (g->members & (1u << c)))
                reply(L, c, LAN_MSG_LEAVE, LAN_GONE, id);
        logf_(L, "lan: game %u closed (its creator %d left)", id, console);
        remove_game(L, g);
    } else {
        g->members &= (uint8_t)~(1u << console);
        g->ready   &= (uint8_t)~(1u << console);
        logf_(L, "lan: %d out of game %u", console, g->id);
    }
    if (why != LAN_OK)
        reply(L, console, LAN_MSG_LEAVE, why, 0);
    L->dirty = 1;
}

static void server_console_gone(LanLobby *L, int console)
{
    if (!(L->present & (1u << console)))
        return;
    server_leave_game(L, console, LAN_OK);
    L->present &= (uint8_t)~(1u << console);
    L->carSet  &= (uint8_t)~(1u << console);
    memset(L->name[console], 0, LAN_NAME_MAX);
    logf_(L, "lan: console %d gone", console);
    L->dirty = 1;
}

static void go(LanLobby *L, LanGame *g)
{
    uint8_t b[GO_SIZE];
    int     c, n = 0;

    L->seedMix = L->seedMix * 1664525u + 1013904223u + L->now;
    memset(&L->go, 0, sizeof L->go);
    L->go.game = g->id;
    L->go.seed = L->seedMix;
    L->go.set  = g->set;
    L->go.console[n++] = g->creator;              // the creator first,
    for (c = 0; c < LAN_CONSOLES; c++)            // then by console id
        if (c != g->creator && (g->members & (1u << c)))
            L->go.console[n++] = (uint8_t)c;
    L->go.racers = n;
    g->started   = 1;
    g->countdown = 0;
    L->countdownEnd[g - L->games] = 0;

    b[0] = g->id;
    b[1] = (uint8_t)(L->go.seed & 0xff);
    b[2] = (uint8_t)(L->go.seed >> 8 & 0xff);
    b[3] = (uint8_t)(L->go.seed >> 16 & 0xff);
    b[4] = (uint8_t)(L->go.seed >> 24);
    b[5] = (uint8_t)n;
    memset(b + 6, 0xff, LAN_CONSOLES);
    memcpy(b + 6, L->go.console, (size_t)n);
    put_settings(b + 6 + LAN_CONSOLES, &g->set);
    for (c = 0; c < n; c++) {
        if (L->go.console[c] == 0)
            push_event(L, LAN_EV_GO, g->id, LAN_OK);
        else
            send_to(L, L->go.console[c], LAN_MSG_GO, b, sizeof b);
    }
    logf_(L, "lan: game %u GO, %d racers, seed %08lx", g->id, n,
          (unsigned long)L->go.seed);
    L->dirty = 1;
}

// FUN_004bb6e0 at the countdown's end, or at once with a racer not
// ready: the unready are dropped, and two must be left.
static void start_now(LanLobby *L, LanGame *g, int asker)
{
    uint8_t unready = (uint8_t)(g->members & ~g->ready);
    int     c;

    g->countdown = 0;
    L->countdownEnd[g - L->games] = 0;
    if (L->racing) {                            // PORT: one race at a time
        reply(L, asker, LAN_MSG_START, LAN_BUSY, g->id);
        L->dirty = 1;
        return;
    }
    if (count_bits((uint8_t)(g->members & g->ready)) < 2) {
        reply(L, asker, LAN_MSG_START, LAN_NOT_ENOUGH, g->id);
        L->dirty = 1;
        return;
    }
    for (c = 0; c < LAN_CONSOLES; c++)
        if (unready & (1u << c))
            server_leave_game(L, c, LAN_REMOVED);
    go(L, g);
}

static void server_handle(LanLobby *L, int from, uint8_t type,
                          const uint8_t *d, size_t n)
{
    LanGame *g;
    int      c;

    if (type != LAN_MSG_HELLO && !(L->present & (1u << from)))
        return;                                 // nothing before its hello
    switch (type) {
    case LAN_MSG_HELLO: {
        uint8_t b[1 + LAN_NAME_MAX];
        char    nm[LAN_NAME_MAX + 1];
        if (L->present & (1u << from))          // its place taken again
            server_console_gone(L, from);
        memset(nm, 0, sizeof nm);
        memcpy(nm, d, n < LAN_NAME_MAX ? n : LAN_NAME_MAX);
        copy_name(L->name[from], nm);
        L->present |= (uint8_t)(1u << from);
        b[0] = (uint8_t)from;
        memcpy(b + 1, L->serverName, LAN_NAME_MAX);
        send_to(L, from, LAN_MSG_WELCOME, b, sizeof b);
        logf_(L, "lan: console %d is %s", from, L->name[from]);
        L->dirty = 1;
        break;
    }
    case LAN_MSG_CAR:
        if (n >= 4) {
            L->car[from] = (uint32_t)d[0] | (uint32_t)d[1] << 8 |
                           (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
            if (L->car[from])
                L->carSet |= (uint8_t)(1u << from);
            else
                L->carSet &= (uint8_t)~(1u << from);
        }
        break;
    case LAN_MSG_CREATE: {
        LanSettings s;
        if (n < LAN_SETTINGS_WIRE + LAN_PASSWORD_MAX)
            break;
        if (game_of(L, from) || L->numGames == LAN_MAX_GAMES) {
            reply(L, from, LAN_MSG_CREATE, LAN_BUSY, 0);
            break;
        }
        get_settings(&s, d);
        clamp_settings(&s);
        if (++L->nextId == 0)
            L->nextId = 1;
        while (find_game(L, L->nextId))
            if (++L->nextId == 0)
                L->nextId = 1;
        g = &L->games[L->numGames];
        memset(g, 0, sizeof *g);
        g->id      = L->nextId;
        g->creator = (uint8_t)from;
        g->members = (uint8_t)(1u << from);
        g->ready   = (uint8_t)(1u << from);     // Ready_Indicator_1: "Ready"
        g->set     = s;
        copy_password(L->password[L->numGames], (const char *)d + LAN_SETTINGS_WIRE);
        g->set.locked = L->password[L->numGames][0] != 0;
        L->countdownEnd[L->numGames] = 0;
        L->numGames++;
        logf_(L, "lan: %d made game %u, mode %u track %u, %d max%s", from,
              g->id, s.mode, s.track, s.maxPlayers, g->set.locked ? ", locked" : "");
        reply(L, from, LAN_MSG_CREATE, LAN_OK, g->id);
        L->dirty = 1;
        break;
    }
    case LAN_MSG_JOIN: {
        char pw[LAN_PASSWORD_MAX];
        LanResult r = LAN_OK;
        if (n < 1 + LAN_PASSWORD_MAX)
            break;
        g = find_game(L, d[0]);
        memset(pw, 0, sizeof pw);
        memcpy(pw, d + 1, LAN_PASSWORD_MAX - 1);
        if (!g)
            r = LAN_GONE;
        else if (game_of(L, from))
            r = LAN_BUSY;
        else if (g->started)
            r = LAN_STARTED;
        else if (count_bits(g->members) >= g->set.maxPlayers)
            r = LAN_FULL;
        else if (g->set.locked && strcmp(pw, L->password[g - L->games]) != 0)
            r = LAN_WRONG_PASSWORD;
        else if (g->set.restriction && (L->carSet & (1u << g->creator)) &&
                 (!(L->carSet & (1u << from)) || L->car[from] != L->car[g->creator]))
            r = LAN_INVALID_CAR;
        if (r != LAN_OK) {
            reply(L, from, LAN_MSG_JOIN, r, d[0]);
            break;
        }
        g->members |= (uint8_t)(1u << from);
        logf_(L, "lan: %d joined game %u", from, g->id);
        reply(L, from, LAN_MSG_JOIN, LAN_OK, g->id);
        L->dirty = 1;
        break;
    }
    case LAN_MSG_LEAVE:
        server_leave_game(L, from, LAN_OK);
        break;
    case LAN_MSG_READY:
        g = game_of(L, from);
        if (!g || g->started || g->creator == from || n < 1)
            break;
        if (d[0])
            g->ready |= (uint8_t)(1u << from);
        else
            g->ready &= (uint8_t)~(1u << from);
        L->dirty = 1;
        break;
    case LAN_MSG_START:
        g = game_of(L, from);
        if (!g || g->started)
            break;
        if (g->creator != from) {
            reply(L, from, LAN_MSG_START, LAN_NOT_CREATOR, g->id);
            break;
        }
        if (L->countdownEnd[g - L->games])
            break;                              // one runs already
        if (L->racing) {                        // PORT: one race at a time
            reply(L, from, LAN_MSG_START, LAN_BUSY, g->id);
            break;
        }
        if ((g->members & ~g->ready) == 0 && count_bits(g->members) >= 2) {
            L->countdownEnd[g - L->games] = L->now + LAN_COUNTDOWN_MS;
            if (L->countdownEnd[g - L->games] == 0)
                L->countdownEnd[g - L->games] = 1;
            g->countdown = (uint8_t)((LAN_COUNTDOWN_MS + 999) / 1000);
            logf_(L, "lan: game %u counting down", g->id);
            L->dirty = 1;
        } else {
            start_now(L, g, from);
        }
        break;
    case LAN_MSG_KICK:
        g = game_of(L, from);
        if (!g || n < 1 || d[0] >= LAN_CONSOLES)
            break;
        c = d[0];
        if (g->creator != from) {
            reply(L, from, LAN_MSG_KICK, LAN_NOT_CREATOR, g->id);
            break;
        }
        if (c != from && (g->members & (1u << c)) && !g->started)
            server_leave_game(L, c, LAN_REMOVED);
        break;
    default:
        break;
    }
}

static void server_frame(LanLobby *L)
{
    WlMsg m;
    int   i, peers = L->net->peerCount(L->net->ctx);

    for (i = 0; i < peers && i + 1 < LAN_CONSOLES; i++) {
        const int c = i + 1;
        if (!L->net->peerAlive(L->net->ctx, i)) {
            server_console_gone(L, c);
            continue;
        }
        while (L->net->recv(L->net->ctx, i, &m)) {
            if (m.type >= LAN_MSG_OTHER_FIRST) {
                if (L->other) L->other(L->otherCtx, c, m.type, m.data, m.len);
                continue;
            }
            server_handle(L, c, m.type, m.data, m.len);
        }
    }
    for (i = 0; i < L->numGames; i++) {
        LanGame *g = &L->games[i];
        uint32_t end = L->countdownEnd[i];
        if (!end || g->started)
            continue;
        if ((int32_t)(L->now - end) >= 0) {
            start_now(L, g, g->creator);    // drops only the unready, never
            continue;                       // the creator: the list stays
        }
        {
            uint8_t left = (uint8_t)((end - L->now + 999) / 1000);
            if (left != g->countdown) {
                g->countdown = left;
                L->dirty = 1;
            }
        }
    }
    if (L->dirty)
        broadcast_lists(L);
}

// ------------------------------------------------------------ a client

static void apply_result(LanLobby *L, uint8_t op, LanResult code, uint8_t game)
{
    switch (op) {
    case LAN_MSG_CREATE:
    case LAN_MSG_JOIN:
        if (code == LAN_OK) {
            L->inGame = game;
            push_event(L, LAN_EV_ENTERED, game, LAN_OK);
        } else {
            push_event(L, LAN_EV_REFUSED, game, code);
        }
        break;
    case LAN_MSG_LEAVE:                         // the server put it out
        if (L->inGame) {
            uint8_t was = L->inGame;
            L->inGame = 0;
            push_event(L, LAN_EV_CLOSED, was, code);
        }
        break;
    default:                                    // start, kick
        push_event(L, LAN_EV_REFUSED, game, code);
        break;
    }
}

static void client_handle(LanLobby *L, uint8_t type, const uint8_t *d, size_t n)
{
    int i;

    switch (type) {
    case LAN_MSG_WELCOME:
        if (n < 1 + LAN_NAME_MAX || d[0] == 0 || d[0] >= LAN_CONSOLES)
            break;
        L->self = d[0];
        memcpy(L->serverName, d + 1, LAN_NAME_MAX);
        L->serverName[LAN_NAME_MAX - 1] = 0;
        logf_(L, "lan: welcomed by %s as console %d", L->serverName, L->self);
        push_event(L, LAN_EV_SERVER_UP, 0, LAN_OK);
        break;
    case LAN_MSG_NAMES:
        if (n < NAMES_SIZE)
            break;
        L->present = d[0];
        memcpy(L->name, d + 1, LAN_CONSOLES * LAN_NAME_MAX);
        for (i = 0; i < LAN_CONSOLES; i++)
            L->name[i][LAN_NAME_MAX - 1] = 0;
        push_event(L, LAN_EV_LISTS, 0, LAN_OK);
        break;
    case LAN_MSG_GAMES: {
        const uint8_t *p = d + 1;
        int k;
        if (n < 1 || d[0] > LAN_MAX_GAMES || n < 1 + (size_t)d[0] * GAME_WIRE)
            break;
        memset(L->games, 0, sizeof L->games);
        L->numGames = d[0];
        for (k = 0; k < L->numGames; k++, p += GAME_WIRE) {
            LanGame *g = &L->games[k];
            g->id        = p[0];
            g->creator   = p[1];
            g->members   = p[2];
            g->ready     = p[3];
            g->started   = p[4];
            g->countdown = p[5];
            get_settings(&g->set, p + 6);
        }
        push_event(L, LAN_EV_LISTS, 0, LAN_OK);
        break;
    }
    case LAN_MSG_RESULT:
        if (n >= 3)
            apply_result(L, d[0], (LanResult)d[1], d[2]);
        break;
    case LAN_MSG_GO:
        if (n < GO_SIZE || d[5] < 2 || d[5] > LAN_CONSOLES)
            break;
        memset(&L->go, 0, sizeof L->go);
        L->go.game   = d[0];
        L->go.seed   = (uint32_t)d[1] | (uint32_t)d[2] << 8 |
                       (uint32_t)d[3] << 16 | (uint32_t)d[4] << 24;
        L->go.racers = d[5];
        memcpy(L->go.console, d + 6, (size_t)L->go.racers);
        get_settings(&L->go.set, d + 6 + LAN_CONSOLES);
        push_event(L, LAN_EV_GO, L->go.game, LAN_OK);
        break;
    default:
        break;
    }
}

static void client_frame(LanLobby *L)
{
    WlMsg m;
    WirelessState st = L->net->state(L->net->ctx);

    if (st != WIRELESS_CONNECTED || !L->net->peerAlive(L->net->ctx, 0)) {
        logf_(L, "lan: the server is gone");
        lanlobby_leave_server(L);
        push_event(L, LAN_EV_SERVER_LOST, 0, LAN_OK);
        return;
    }
    while (L->on && L->net->recv(L->net->ctx, 0, &m)) {
        if (m.type >= LAN_MSG_OTHER_FIRST) {
            if (L->other) L->other(L->otherCtx, 0, m.type, m.data, m.len);
            continue;
        }
        client_handle(L, m.type, m.data, m.len);
    }
}

// ------------------------------------------------------------ requests

static void request(LanLobby *L, uint8_t type, const void *d, size_t n)
{
    if (!L->on)
        return;
    if (L->server)
        server_handle(L, 0, type, (const uint8_t *)d, n);
    else
        send_to(L, 0, type, d, n);
}

void lanlobby_create(LanLobby *L, const LanSettings *s, const char *password)
{
    uint8_t b[LAN_SETTINGS_WIRE + LAN_PASSWORD_MAX];

    put_settings(b, s);
    copy_password((char *)b + LAN_SETTINGS_WIRE, password);
    request(L, LAN_MSG_CREATE, b, sizeof b);
}

void lanlobby_join_game(LanLobby *L, uint8_t game, const char *password)
{
    uint8_t b[1 + LAN_PASSWORD_MAX];

    b[0] = game;
    copy_password((char *)b + 1, password);
    request(L, LAN_MSG_JOIN, b, sizeof b);
}

void lanlobby_leave_game(LanLobby *L)
{
    if (!L->inGame)
        return;
    L->inGame = 0;                              // out at once on this side
    request(L, LAN_MSG_LEAVE, NULL, 0);
}

void lanlobby_set_ready(LanLobby *L, int ready)
{
    uint8_t b = ready ? 1 : 0;

    request(L, LAN_MSG_READY, &b, 1);
}

void lanlobby_start(LanLobby *L)
{
    request(L, LAN_MSG_START, NULL, 0);
}

void lanlobby_kick(LanLobby *L, int console)
{
    uint8_t b = (uint8_t)console;

    request(L, LAN_MSG_KICK, &b, 1);
}

void lanlobby_set_car(LanLobby *L, uint32_t carClass)
{
    uint8_t b[4] = { (uint8_t)carClass, (uint8_t)(carClass >> 8),
                     (uint8_t)(carClass >> 16), (uint8_t)(carClass >> 24) };

    request(L, LAN_MSG_CAR, b, 4);
}

// ------------------------------------------------------------ the server's life

void lanlobby_init(LanLobby *L, const LanNet *net, LanLogFn log)
{
    memset(L, 0, sizeof *L);
    L->net  = net;
    L->log  = log;
    L->self = -1;
}

void lanlobby_set_other(LanLobby *L, LanOtherFn fn, void *ctx)
{
    L->other    = fn;
    L->otherCtx = ctx;
}

void lanlobby_set_racing(LanLobby *L, int racing)
{
    L->racing = racing;
}

static void reset_lists(LanLobby *L)
{
    memset(L->name, 0, sizeof L->name);
    memset(L->games, 0, sizeof L->games);
    memset(L->password, 0, sizeof L->password);
    memset(L->countdownEnd, 0, sizeof L->countdownEnd);
    memset(L->car, 0, sizeof L->car);
    L->numGames = 0;
    L->present = L->carSet = 0;
    L->inGame = 0;
    L->dirty = 0;
}

int lanlobby_host(LanLobby *L, const char *name, uint32_t nowMs)
{
    uint8_t app[APPDATA_SIZE];

    lanlobby_leave_server(L);
    L->now = nowMs;
    copy_name(L->selfName, name);
    if (!L->net->start(L->net->ctx))
        return 0;
    memset(app, 0, sizeof app);
    app[0] = APPDATA_TAG;
    app[1] = APPDATA_VERSION;
    memcpy(app + 2, L->selfName, LAN_NAME_MAX);
    if (!L->net->host(L->net->ctx, LAN_CONSOLES, app, sizeof app)) {
        L->net->stop(L->net->ctx);
        return 0;
    }
    reset_lists(L);
    L->on     = 1;
    L->server = 1;
    L->self   = 0;
    memcpy(L->serverName, L->selfName, LAN_NAME_MAX);
    memcpy(L->name[0], L->selfName, LAN_NAME_MAX);
    L->present = 1;
    L->seedMix ^= nowMs * 2654435761u;
    logf_(L, "lan: hosting \"%s\"", L->serverName);
    push_event(L, LAN_EV_SERVER_UP, 0, LAN_OK);
    push_event(L, LAN_EV_LISTS, 0, LAN_OK);
    return 1;
}

int lanlobby_scan(LanLobby *L, LanServer *out, int max)
{
    WirelessHost h[LAN_SCAN_MAX];
    int          i, n, k = 0;

    if (!L->on && !L->net->start(L->net->ctx))
        return -1;
    n = L->net->scan(L->net->ctx, h, LAN_SCAN_MAX);
    for (i = 0; i < n && k < max; i++) {
        if (h[i].appdataSize < APPDATA_SIZE || h[i].appdata[0] != APPDATA_TAG ||
            h[i].appdata[1] != APPDATA_VERSION)
            continue;
        memset(&out[k], 0, sizeof out[k]);
        memcpy(out[k].name, h[i].appdata + 2, LAN_NAME_MAX);
        out[k].name[LAN_NAME_MAX - 1] = 0;
        out[k].consoles    = h[i].consoles;
        out[k].maxConsoles = h[i].maxConsoles;
        out[k].scanIndex   = h[i].scanIndex;
        k++;
    }
    return n < 0 ? -1 : k;
}

int lanlobby_join_server(LanLobby *L, int scanIndex, const char *name,
                         uint32_t nowMs)
{
    L->now = nowMs;
    copy_name(L->selfName, name);
    if (!L->net->join(L->net->ctx, scanIndex))
        return 0;
    reset_lists(L);
    L->on     = 1;
    L->server = 0;
    L->self   = -1;
    send_to(L, 0, LAN_MSG_HELLO, L->selfName, LAN_NAME_MAX);
    logf_(L, "lan: joining as \"%s\"", L->selfName);
    return 1;
}

void lanlobby_leave_server(LanLobby *L)
{
    if (L->on) {
        L->net->leave(L->net->ctx);
        logf_(L, "lan: off the server");
    }
    if (L->on || L->net->state(L->net->ctx) != WIRELESS_OFF)
        L->net->stop(L->net->ctx);
    L->on     = 0;
    L->server = 0;
    L->self   = -1;
    reset_lists(L);
}

void lanlobby_frame(LanLobby *L, uint32_t nowMs)
{
    L->now = nowMs;
    if (!L->on)
        return;
    if (L->server) {
        if (L->net->state(L->net->ctx) != WIRELESS_HOSTING) {
            logf_(L, "lan: the network is gone");
            lanlobby_leave_server(L);
            push_event(L, LAN_EV_SERVER_LOST, 0, LAN_OK);
            return;
        }
        server_frame(L);
    } else {
        client_frame(L);
    }
}
