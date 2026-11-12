// Host harness for lanlobby.c (TASKS #321b): six consoles in one process
// over a fake star network that delivers every message once and in order
// (wlproto's job, checked by wlproto_test.c), and the lobby's rules checked
// scenario by scenario. Prints "faults N"; tools/lanlobby_check.py runs it.
//
// The fake keeps wireless.c's place rules: a client's place stays when it
// goes; the next to come takes a new place while there are fewer than
// five, then the first place of one gone.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lanlobby.h"

#define N LAN_CONSOLES
#define Q 256

typedef struct { uint8_t type, len; uint8_t d[WL_MSG_MAX]; } Msg;
typedef struct { Msg m[Q]; int head, count; } Queue;

typedef struct Fake {
    int           id;            // the harness's console index
    int           up;            // UDS started
    WirelessState st;
    int           hostOf;        // a client's host's index, -1
    int           peer[N - 1];   // a host's peers: console index, -1 none
    int           alive[N - 1];
    int           numPeers;
    uint8_t       app[WIRELESS_APPDATA_MAX];
    size_t        appN;
    int           maxConsoles;
    int           deaf;          // a client: its host's link timed out
} Fake;

static Fake      F[N];
static Queue     q[N][N];       // q[to][from]
static LanLobby  L[N];
static LanNet    net[N];
static int       faults;
static uint32_t  now = 1000;
static int       verbose;

#define FAULT(...) do { faults++; printf("  FAULT %s:%d ", __func__, __LINE__); \
                        printf(__VA_ARGS__); printf("\n"); } while (0)

static void lg(const char *line) { if (verbose) printf("    %s\n", line); }

// ------------------------------------------------------------ the fake

static int f_start(void *c) { Fake *f = c; f->up = 1; if (f->st == WIRELESS_OFF) f->st = WIRELESS_IDLE; return 1; }
static void f_stop(void *c) { Fake *f = c; f->up = 0; f->st = WIRELESS_OFF; }

static int f_host(void *c, int consoles, const void *a, size_t n)
{
    Fake *f = c;
    int   i;

    if (!f->up)
        return 0;
    f->st = WIRELESS_HOSTING;
    memcpy(f->app, a, n);
    f->appN = n;
    f->maxConsoles = consoles;
    f->numPeers = 0;
    for (i = 0; i < N - 1; i++) { f->peer[i] = -1; f->alive[i] = 0; }
    return 1;
}

static int f_scan(void *c, WirelessHost *out, int max)
{
    int i, k = 0;
    (void)c;
    for (i = 0; i < N && k < max; i++) {
        Fake *h = &F[i];
        int alive = 0, j;
        if (h->st != WIRELESS_HOSTING)
            continue;
        for (j = 0; j < h->numPeers; j++)
            alive += h->alive[j];
        memset(&out[k], 0, sizeof out[k]);
        snprintf(out[k].name, sizeof out[k].name, "console%d", i);
        memcpy(out[k].appdata, h->app, h->appN);
        out[k].appdataSize = h->appN;
        out[k].consoles = 1 + alive;
        out[k].maxConsoles = h->maxConsoles;
        out[k].scanIndex = i;
        k++;
    }
    return k;
}

static void clear_queue(int to, int from) { q[to][from].head = q[to][from].count = 0; }

static int f_join(void *c, int scanIndex)
{
    Fake *f = c, *h;
    int   slot = -1, j;

    if (scanIndex < 0 || scanIndex >= N)
        return 0;
    h = &F[scanIndex];
    if (h->st != WIRELESS_HOSTING || !f->up)
        return 0;
    if (h->numPeers < N - 1)
        slot = h->numPeers++;
    else
        for (j = 0; j < h->numPeers && slot < 0; j++)
            if (!h->alive[j])
                slot = j;
    if (slot < 0)
        return 0;
    h->peer[slot] = f->id;
    h->alive[slot] = 1;
    f->st = WIRELESS_CONNECTED;
    f->hostOf = scanIndex;
    clear_queue(f->id, scanIndex);
    clear_queue(scanIndex, f->id);
    return 1;
}

static void f_leave(void *c)
{
    Fake *f = c;
    int   i, j;

    if (f->st == WIRELESS_CONNECTED) {
        Fake *h = &F[f->hostOf];
        for (j = 0; j < h->numPeers; j++)
            if (h->peer[j] == f->id)
                h->alive[j] = 0;
        f->hostOf = -1;
    } else if (f->st == WIRELESS_HOSTING) {
        for (i = 0; i < N; i++)
            if (F[i].st == WIRELESS_CONNECTED && F[i].hostOf == f->id)
                F[i].st = WIRELESS_LOST;
    }
    if (f->up)
        f->st = WIRELESS_IDLE;
}

static WirelessState f_state(void *c) { return ((Fake *)c)->st; }

static int f_peerCount(void *c)
{
    Fake *f = c;
    return f->st == WIRELESS_HOSTING ? f->numPeers : f->st == WIRELESS_CONNECTED ? 1 : 0;
}

static int f_peerAlive(void *c, int i)
{
    Fake *f = c;
    if (f->st == WIRELESS_HOSTING)
        return i >= 0 && i < f->numPeers && f->alive[i];
    return f->st == WIRELESS_CONNECTED && i == 0 && !f->deaf;
}

static int peer_index(Fake *f, int i)
{
    if (f->st == WIRELESS_HOSTING)
        return (i >= 0 && i < f->numPeers && f->alive[i]) ? f->peer[i] : -1;
    if (f->st == WIRELESS_CONNECTED && i == 0)
        return f->hostOf;
    return -1;
}

static int f_send(void *c, int i, uint8_t type, const void *d, size_t n)
{
    Fake  *f = c;
    int    to = peer_index(f, i);
    Queue *qq;
    Msg   *m;

    if (to < 0 || n > WL_MSG_MAX)
        return 0;
    qq = &q[to][f->id];
    if (qq->count == Q) {
        printf("  FAULT queue %d<-%d full\n", to, f->id);
        faults++;
        return 0;
    }
    m = &qq->m[(qq->head + qq->count++) % Q];
    m->type = type;
    m->len = (uint8_t)n;
    if (n)
        memcpy(m->d, d, n);
    return 1;
}

static int f_recv(void *c, int i, WlMsg *out)
{
    Fake  *f = c;
    int    from = peer_index(f, i);
    Queue *qq;
    Msg   *m;

    if (from < 0)
        return 0;
    qq = &q[f->id][from];
    if (!qq->count)
        return 0;
    m = &qq->m[qq->head];
    qq->head = (qq->head + 1) % Q;
    qq->count--;
    out->seq = 0;
    out->type = m->type;
    out->len = m->len;
    memcpy(out->data, m->d, m->len);
    return 1;
}

// ------------------------------------------------------------ helpers

static LanEvent evs[N][64];
static int      nev[N];

static void reset(void)
{
    int i;

    memset(F, 0, sizeof F);
    memset(q, 0, sizeof q);
    for (i = 0; i < N; i++) {
        F[i].id = i;
        F[i].hostOf = -1;
        net[i] = (LanNet){ &F[i], f_start, f_stop, f_host, f_scan, f_join,
                           f_leave, f_state, f_peerCount, f_peerAlive, f_send, f_recv };
        lanlobby_init(&L[i], &net[i], lg);
        nev[i] = 0;
    }
}

static void frames(int count, uint32_t ms)
{
    int k, i;
    LanEvent e;

    for (k = 0; k < count; k++) {
        now += ms;
        for (i = 0; i < N; i++) {
            lanlobby_frame(&L[i], now);
            while (lanlobby_event(&L[i], &e))
                if (nev[i] < 64)
                    evs[i][nev[i]++] = e;
        }
    }
}

static void clear_events(void) { memset(nev, 0, sizeof nev); }

// The last event of `kind` console i saw since clear_events, or NULL.
static const LanEvent *saw(int i, LanEventKind kind)
{
    int k;
    for (k = nev[i] - 1; k >= 0; k--)
        if (evs[i][k].kind == kind)
            return &evs[i][k];
    return NULL;
}

static int count_ev(int i, LanEventKind kind)
{
    int k, n = 0;
    for (k = 0; k < nev[i]; k++)
        n += evs[i][k].kind == kind;
    return n;
}

static const char *NAMES[N] = { "Host", "Alice", "Bob", "Carol", "Dave", "Eve" };

// Host on 0, clients 1..k join; checks the welcome ids and every list.
static void server_with(int k)
{
    LanServer s[LAN_SCAN_MAX];
    int i, n;

    if (!lanlobby_host(&L[0], NAMES[0], now))
        FAULT("host failed");
    for (i = 1; i <= k; i++) {
        n = lanlobby_scan(&L[i], s, LAN_SCAN_MAX);
        if (n != 1 || strcmp(s[0].name, NAMES[0]) != 0)
            FAULT("console %d scan: %d servers, first \"%s\"", i, n, n > 0 ? s[0].name : "");
        if (!lanlobby_join_server(&L[i], s[0].scanIndex, NAMES[i], now))
            FAULT("console %d join failed", i);
        frames(3, 16);
    }
    frames(3, 16);
}

static int mask_of(const LanLobby *l)
{
    return l->present;
}

static uint8_t create(int i, uint8_t mode, int maxP, const char *pw)
{
    LanSettings s;
    const LanEvent *e;

    lanlobby_default_settings(&s, mode, 4001, 0);
    s.maxPlayers = (uint8_t)maxP;
    clear_events();
    lanlobby_create(&L[i], &s, pw);
    frames(3, 16);
    e = saw(i, LAN_EV_ENTERED);
    if (!e) {
        FAULT("console %d create: not entered", i);
        return 0;
    }
    return e->game;
}

static LanResult join(int i, uint8_t game, const char *pw)
{
    const LanEvent *e;

    clear_events();
    lanlobby_join_game(&L[i], game, pw);
    frames(3, 16);
    if (saw(i, LAN_EV_ENTERED))
        return LAN_OK;
    e = saw(i, LAN_EV_REFUSED);
    return e ? e->result : (LanResult)99;
}

// Every console's view of game `id` against the server's.
static void lists_agree(void)
{
    int i, k;

    for (i = 1; i < N; i++) {
        if (!L[i].on)
            continue;
        if (L[i].present != L[0].present)
            FAULT("console %d present %02x, server %02x", i, L[i].present, L[0].present);
        if (L[i].numGames != L[0].numGames) {
            FAULT("console %d has %d games, server %d", i, L[i].numGames, L[0].numGames);
            continue;
        }
        for (k = 0; k < L[0].numGames; k++)
            if (memcmp(&L[i].games[k], &L[0].games[k], sizeof L[0].games[k]) != 0)
                FAULT("console %d game %d differs", i, k);
        for (k = 0; k < N; k++)
            if ((L[0].present & (1u << k)) && strcmp(L[i].name[k], L[0].name[k]) != 0)
                FAULT("console %d name %d \"%s\" vs \"%s\"", i, k, L[i].name[k], L[0].name[k]);
    }
}

// ------------------------------------------------------------ scenarios

static void s_join_six(void)
{
    int i;

    reset();
    server_with(5);
    for (i = 1; i < N; i++)
        if (L[i].self != i)
            FAULT("console %d welcomed as %d", i, L[i].self);
    if (mask_of(&L[0]) != 0x3f)
        FAULT("server present %02x", L[0].present);
    for (i = 0; i < N; i++)
        if (strcmp(L[1].name[i], NAMES[i]) != 0)
            FAULT("name %d \"%s\"", i, L[1].name[i]);
    lists_agree();
}

static void s_password_full(void)
{
    uint8_t g;

    reset();
    server_with(5);
    g = create(1, 0, 3, "abc");
    if (!L[0].games[0].set.locked)
        FAULT("game not locked");
    if (join(2, g, "abd") != LAN_WRONG_PASSWORD)
        FAULT("wrong password let in");
    if (join(2, g, "") != LAN_WRONG_PASSWORD)
        FAULT("no password let in");
    if (join(2, g, "abc") != LAN_OK)
        FAULT("right password refused");
    if (join(3, g, "abc") != LAN_OK)
        FAULT("third refused");
    if (join(4, g, "abc") != LAN_FULL)
        FAULT("fourth let into a game of three");
    if (join(3, g, "abc") != LAN_BUSY)
        FAULT("joined twice");
    if (join(4, 200, "") != LAN_GONE)
        FAULT("joined a game that is not there");
    if (L[2].inGame != g || L[3].inGame != g || L[4].inGame)
        FAULT("inGame %u %u %u", L[2].inGame, L[3].inGame, L[4].inGame);
    lists_agree();

    // Two locked games; the first closes and the second keeps its password.
    {
        uint8_t g2 = create(5, 0, 6, "second");
        if (join(4, g2, "abc") != LAN_WRONG_PASSWORD)
            FAULT("the first game's password opened the second");
        lanlobby_leave_game(&L[1]);
        frames(3, 16);
        if (L[0].numGames != 1 || L[0].games[0].id != g2)
            FAULT("games after the close: %d", L[0].numGames);
        if (join(4, g2, "abc") != LAN_WRONG_PASSWORD)
            FAULT("the closed game's password opened the next");
        if (join(4, g2, "second") != LAN_OK)
            FAULT("the second game's password lost");
    }
}

static void s_countdown_go(void)
{
    uint8_t g;
    const LanEvent *e;
    int i;

    reset();
    server_with(5);
    g = create(0, 4, 6, NULL);           // the server plays: URL, six
    if (join(5, g, NULL) != LAN_OK || join(2, g, NULL) != LAN_OK)
        FAULT("joins refused");
    clear_events();
    lanlobby_start(&L[5]);
    frames(3, 16);
    e = saw(5, LAN_EV_REFUSED);
    if (!e || e->result != LAN_NOT_CREATOR)
        FAULT("a racer started the game");
    lanlobby_set_ready(&L[0], 0);        // the creator: always ready
    lanlobby_kick(&L[0], 0);             // nor kicks itself
    lanlobby_set_ready(&L[5], 1);
    lanlobby_set_ready(&L[2], 1);
    frames(3, 16);
    if (L[0].numGames != 1 || L[0].inGame != g)
        FAULT("the creator kicked itself out");
    if (L[3].games[0].ready != (1u | 1u << 5 | 1u << 2))
        FAULT("ready %02x", L[3].games[0].ready);
    clear_events();
    lanlobby_start(&L[0]);
    frames(3, 16);
    if (L[0].games[0].countdown != 5 || L[4].games[0].countdown != 5)
        FAULT("countdown %u / %u", L[0].games[0].countdown, L[4].games[0].countdown);
    if (saw(0, LAN_EV_GO) || saw(5, LAN_EV_GO))
        FAULT("GO before the countdown ran");
    frames(200, 16);                     // 3.2 s
    if (saw(0, LAN_EV_GO))
        FAULT("GO early");
    if (L[4].games[0].countdown != 2)
        FAULT("countdown shown %u, 2 s left", L[4].games[0].countdown);
    frames(150, 16);                     // past 5 s
    for (i = 0; i < N; i++) {
        const int racer = i == 0 || i == 5 || i == 2;
        if (!!saw(i, LAN_EV_GO) != racer)
            FAULT("console %d GO %d, racer %d", i, !!saw(i, LAN_EV_GO), racer);
        if (count_ev(i, LAN_EV_GO) > 1)
            FAULT("console %d GO twice", i);
    }
    if (L[5].go.seed != L[0].go.seed || L[2].go.seed != L[0].go.seed)
        FAULT("seeds differ");
    if (L[2].go.racers != 3 || L[2].go.console[0] != 0 || L[2].go.console[1] != 2 ||
        L[2].go.console[2] != 5)
        FAULT("grid %d: %u %u %u", L[2].go.racers, L[2].go.console[0],
              L[2].go.console[1], L[2].go.console[2]);
    if (memcmp(&L[5].go.set, &L[0].games[0].set, sizeof L[0].go.set) != 0)
        FAULT("settings differ");
    if (L[5].go.set.track != 4001 || L[3].games[0].set.track != 4001 ||
        L[5].go.set.mode != 4 || L[5].go.set.laps != 2)
        FAULT("track %u / %u, mode %u, laps %u", L[5].go.set.track,
              L[3].games[0].set.track, L[5].go.set.mode, L[5].go.set.laps);
    if (join(3, g, NULL) != LAN_STARTED)
        FAULT("joined a started game");
    lists_agree();
}

static void s_unready_dropped(void)
{
    uint8_t g;
    const LanEvent *e;

    reset();
    server_with(5);
    g = create(1, 0, 6, NULL);
    join(2, g, NULL);
    join(3, g, NULL);
    lanlobby_set_ready(&L[2], 1);
    frames(3, 16);
    clear_events();
    lanlobby_start(&L[1]);               // 3 not ready: at once, 3 dropped
    frames(3, 16);
    e = saw(3, LAN_EV_CLOSED);
    if (!e || e->result != LAN_REMOVED)
        FAULT("the unready racer was not removed");
    if (saw(3, LAN_EV_GO) || !saw(1, LAN_EV_GO) || !saw(2, LAN_EV_GO))
        FAULT("GO went to the wrong consoles");
    if (L[1].go.racers != 2)
        FAULT("racers %d", L[1].go.racers);

    // None ready but the creator: not enough, no one dropped.
    g = create(4, 2, 6, NULL);
    join(5, g, NULL);
    clear_events();
    lanlobby_start(&L[4]);
    frames(3, 16);
    e = saw(4, LAN_EV_REFUSED);
    if (!e || e->result != LAN_NOT_ENOUGH)
        FAULT("started with one racer");
    if (L[5].inGame != g || saw(5, LAN_EV_CLOSED))
        FAULT("the racer was dropped by a start that failed");
    // Alone: not enough.
    lanlobby_leave_game(&L[5]);
    frames(3, 16);
    clear_events();
    lanlobby_start(&L[4]);
    frames(3, 16);
    e = saw(4, LAN_EV_REFUSED);
    if (!e || e->result != LAN_NOT_ENOUGH)
        FAULT("started alone");
    lists_agree();
}

static void s_countdown_unready_join(void)
{
    uint8_t g;
    const LanEvent *e;

    reset();
    server_with(4);
    g = create(3, 1, 6, NULL);
    join(1, g, NULL);
    lanlobby_set_ready(&L[1], 1);
    frames(3, 16);
    lanlobby_start(&L[3]);
    frames(30, 16);
    clear_events();
    if (join(2, g, NULL) != LAN_OK)      // comes during the countdown
        FAULT("refused during the countdown");
    lanlobby_start(&L[3]);               // a second press: one runs already
    frames(3, 16);
    if (saw(3, LAN_EV_GO) || saw(2, LAN_EV_CLOSED) || !L[0].games[0].countdown)
        FAULT("the second press started at once");
    frames(400, 16);
    e = saw(2, LAN_EV_CLOSED);
    if (!e || e->result != LAN_REMOVED)
        FAULT("the late, unready racer was not dropped");
    if (!saw(3, LAN_EV_GO) || !saw(1, LAN_EV_GO) || count_ev(3, LAN_EV_GO) != 1)
        FAULT("GO %d %d", count_ev(3, LAN_EV_GO), count_ev(1, LAN_EV_GO));
    if (L[1].go.racers != 2 || L[1].go.console[0] != 3 || L[1].go.console[1] != 1)
        FAULT("grid %d: %u %u", L[1].go.racers, L[1].go.console[0], L[1].go.console[1]);
    lists_agree();
}

static void s_creator_leaves(void)
{
    uint8_t g;
    const LanEvent *e;

    reset();
    server_with(3);
    g = create(2, 3, 6, NULL);
    join(1, g, NULL);
    join(0, g, NULL);
    clear_events();
    lanlobby_leave_game(&L[2]);
    frames(3, 16);
    e = saw(1, LAN_EV_CLOSED);
    if (!e || e->result != LAN_GONE || L[1].inGame)
        FAULT("console 1 not told the game closed");
    e = saw(0, LAN_EV_CLOSED);
    if (!e || e->result != LAN_GONE || L[0].inGame)
        FAULT("the server not told the game closed");
    if (L[0].numGames || L[3].numGames)
        FAULT("games left %d %d", L[0].numGames, L[3].numGames);
    if (saw(2, LAN_EV_CLOSED))
        FAULT("the creator told its own leave");
    if (L[2].inGame)
        FAULT("the creator still in its game");

    // A racer leaves: the game stays.
    g = create(2, 3, 6, NULL);
    join(1, g, NULL);
    lanlobby_set_ready(&L[1], 1);
    frames(3, 16);
    clear_events();
    lanlobby_leave_game(&L[1]);
    frames(3, 16);
    if (L[0].numGames != 1 || L[0].games[0].members != (1u << 2))
        FAULT("members %02x", L[0].games[0].members);
    join(1, g, NULL);
    if (L[3].games[0].ready != (1u << 2))
        FAULT("back in ready: %02x", L[3].games[0].ready);
    lists_agree();
}

static void s_console_lost(void)
{
    uint8_t g;
    const LanEvent *e;
    LanServer s[LAN_SCAN_MAX];

    reset();
    server_with(5);
    g = create(3, 0, 6, NULL);
    join(4, g, NULL);
    join(1, g, NULL);
    clear_events();
    f_leave(&F[3]);                      // out of range: its link lost
    F[3].st = WIRELESS_LOST;
    frames(3, 16);
    e = saw(4, LAN_EV_CLOSED);
    if (!e || e->result != LAN_GONE)
        FAULT("racers not told the creator went");
    if (L[0].present & (1u << 3))
        FAULT("console 3 still present");
    if (!saw(3, LAN_EV_SERVER_LOST) || L[3].on)
        FAULT("console 3 not told it lost the server");
    if (L[0].name[3][0] || L[5].name[3][0])
        FAULT("console 3's name kept");
    lists_agree();

    // A new console in its place is someone new.
    if (lanlobby_scan(&L[3], s, LAN_SCAN_MAX) != 1)
        FAULT("no server to rejoin");
    lanlobby_join_server(&L[3], s[0].scanIndex, "Zed", now);
    frames(5, 16);
    if (L[3].self != 3 || strcmp(L[0].name[3], "Zed") != 0 || strcmp(L[5].name[3], "Zed") != 0)
        FAULT("place 3: self %d, \"%s\"", L[3].self, L[0].name[3]);
    if (L[3].inGame)
        FAULT("came back in a game");
    lists_agree();

    // The server goes: every client told.
    clear_events();
    lanlobby_leave_server(&L[0]);
    frames(3, 16);
    for (int i = 1; i < N; i++)
        if (!saw(i, LAN_EV_SERVER_LOST) || L[i].on)
            FAULT("console %d not told the server went", i);
}

static void s_evenly_matched(void)
{
    LanSettings s;
    const LanEvent *e;
    uint8_t g;

    reset();
    server_with(5);                      // full: a new console takes a place
    lanlobby_set_car(&L[1], 0x11);
    lanlobby_set_car(&L[2], 0x22);
    lanlobby_set_car(&L[3], 0x11);
    frames(3, 16);
    lanlobby_default_settings(&s, 0, 4001, 0);
    s.restriction = 1;
    clear_events();
    lanlobby_create(&L[1], &s, NULL);
    frames(3, 16);
    e = saw(1, LAN_EV_ENTERED);
    g = e ? e->game : 0;
    if (join(2, g, NULL) != LAN_INVALID_CAR)
        FAULT("another class let into Evenly Matched");
    if (join(3, g, NULL) != LAN_OK)
        FAULT("the same class refused");
    if (join(0, g, NULL) != LAN_INVALID_CAR)
        FAULT("no car let into Evenly Matched");
    {
        LanServer sv[LAN_SCAN_MAX];
        lanlobby_leave_server(&L[3]);
        frames(3, 16);
        lanlobby_scan(&L[3], sv, LAN_SCAN_MAX);
        lanlobby_join_server(&L[3], sv[0].scanIndex, "New", now);
        frames(3, 16);
        if (L[3].self != 3)
            FAULT("rejoined as %d", L[3].self);
        if (join(3, g, NULL) != LAN_INVALID_CAR)
            FAULT("a new console took the old one's car");
    }
}

static void s_settings_clamped(void)
{
    LanSettings s;
    int lo, hi;

    reset();
    server_with(1);
    memset(&s, 0, sizeof s);
    s.mode = 0;
    s.laps = 0;
    s.maxPlayers = 7;
    s.raceBreaker = 7;
    lanlobby_create(&L[1], &s, NULL);
    frames(3, 16);
    if (L[0].games[0].set.laps != 2 || L[0].games[0].set.maxPlayers != 6 ||
        L[0].games[0].set.raceBreaker != 1)
        FAULT("circuit: laps %u max %u breaker %u", L[0].games[0].set.laps,
              L[0].games[0].set.maxPlayers, L[0].games[0].set.raceBreaker);
    lanlobby_leave_game(&L[1]);
    frames(3, 16);
    s.mode = 3;                          // drift: laps from 4, no breaker
    s.laps = 2;
    s.maxPlayers = 1;
    lanlobby_create(&L[1], &s, NULL);
    frames(3, 16);
    if (L[0].games[0].set.laps != 4 || L[0].games[0].set.maxPlayers != 2 ||
        L[0].games[0].set.raceBreaker != 0)
        FAULT("drift: laps %u max %u breaker %u", L[0].games[0].set.laps,
              L[0].games[0].set.maxPlayers, L[0].games[0].set.raceBreaker);
    lanlobby_leave_game(&L[1]);
    frames(3, 16);
    s.mode = 1;                          // sprint: one lap
    s.laps = 5;
    lanlobby_create(&L[1], &s, NULL);
    frames(3, 16);
    if (L[0].games[0].set.laps != 1)
        FAULT("sprint laps %u", L[0].games[0].set.laps);
    lanlobby_lap_range(3, 1, &lo, &hi);
    if (lo != 1 || hi != 1)
        FAULT("point-to-point drift laps %d..%d", lo, hi);
    lanlobby_lap_range(10, 0, &lo, &hi);
    if (lo != 4 || hi != 10)
        FAULT("Street X laps %d..%d", lo, hi);
    lanlobby_lap_range(8, 0, &lo, &hi);
    if (lo != 1 || hi != 1)
        FAULT("Outrun laps %d..%d", lo, hi);
    lanlobby_lap_range(4, 0, &lo, &hi);
    if (lo != 2 || hi != 10)
        FAULT("URL laps %d..%d", lo, hi);
    lanlobby_default_settings(&s, 2, 4001, 0);
    if (s.maxPlayers != 6 || s.laps != 1 || !s.collision)
        FAULT("defaults: max %u laps %u collision %u", s.maxPlayers, s.laps,
              s.collision);
    lanlobby_leave_game(&L[1]);
    frames(3, 16);
    s.mode = 10;                         // Street X: no breaker
    s.raceBreaker = 1;
    lanlobby_create(&L[1], &s, NULL);
    frames(3, 16);
    if (L[0].games[0].set.raceBreaker != 0 || L[0].games[0].set.laps != 4)
        FAULT("Street X: breaker %u laps %u", L[0].games[0].set.raceBreaker,
              L[0].games[0].set.laps);
    lanlobby_leave_game(&L[1]);
    frames(3, 16);
    s.mode = 8;
    s.reverse = 1;
    s.laps = 3;
    lanlobby_create(&L[1], &s, NULL);
    frames(3, 16);
    if (L[0].games[0].set.reverse != 0 || L[0].games[0].set.laps != 1)
        FAULT("Outrun: reverse %u laps %u", L[0].games[0].set.reverse,
              L[0].games[0].set.laps);
}

static void s_kick_and_busy(void)
{
    uint8_t g, g2;
    const LanEvent *e;

    reset();
    server_with(5);
    g = create(1, 0, 6, NULL);
    join(2, g, NULL);
    join(3, g, NULL);
    clear_events();
    lanlobby_kick(&L[2], 3);             // not the creator
    frames(3, 16);
    e = saw(2, LAN_EV_REFUSED);
    if (!e || e->result != LAN_NOT_CREATOR || L[3].inGame != g)
        FAULT("a racer kicked");
    clear_events();
    lanlobby_kick(&L[1], 3);
    frames(3, 16);
    e = saw(3, LAN_EV_CLOSED);
    if (!e || e->result != LAN_REMOVED || L[3].inGame)
        FAULT("the kick did not take");
    // Already in a game: no second.
    clear_events();
    lanlobby_create(&L[2], &L[0].games[0].set, NULL);
    frames(3, 16);
    e = saw(2, LAN_EV_REFUSED);
    if (!e || e->result != LAN_BUSY)
        FAULT("made a game while in one");
    // Every console its own game: six, then the server has no room.
    g2 = create(0, 1, 6, NULL);
    create(3, 1, 6, NULL);
    create(4, 1, 6, NULL);
    create(5, 1, 6, NULL);
    if (L[0].numGames != 5 || !g2)
        FAULT("games %d", L[0].numGames);
    lists_agree();
}

static void s_names(void)
{
    LanServer s[LAN_SCAN_MAX];

    reset();
    lanlobby_host(&L[0], "AVeryLongServerName", now);
    if (strcmp(L[0].serverName, "AVeryLongSer") != 0)
        FAULT("server name \"%s\" not cut to 12", L[0].serverName);
    if (lanlobby_scan(&L[1], s, LAN_SCAN_MAX) != 1 || strcmp(s[0].name, "AVeryLongSer") != 0)
        FAULT("scan name");
    if (s[0].consoles != 1 || s[0].maxConsoles != 6)
        FAULT("consoles %d of %d", s[0].consoles, s[0].maxConsoles);
    lanlobby_join_server(&L[1], s[0].scanIndex, "", now);
    frames(3, 16);
    if (L[1].self != 1 || L[0].name[1][0] != 0)
        FAULT("empty name: self %d \"%s\"", L[1].self, L[0].name[1]);
    // Another port's network is not listed.
    F[2].up = 1;
    f_host(&F[2], 2, "X\001StrangerNetwork", 18);
    if (lanlobby_scan(&L[3], s, LAN_SCAN_MAX) != 1)
        FAULT("a stranger's network listed");
}

static void s_link(void)
{
    uint8_t g, b[LAN_SETTINGS_WIRE + LAN_PASSWORD_MAX];
    LanSettings s;

    reset();
    server_with(3);
    g = create(1, 0, 6, NULL);
    join(3, g, NULL);
    // A hello from a place still held: a new console came into it before
    // the old was seen gone.
    {
        char nm[LAN_NAME_MAX] = "Again";
        f_send(&F[3], 0, LAN_MSG_HELLO, nm, sizeof nm);
    }
    frames(3, 16);
    if (L[0].games[0].members & (1u << 3))
        FAULT("the old console 3 still in the game");
    if (strcmp(L[2].name[3], "Again") != 0)
        FAULT("place 3 is \"%s\"", L[2].name[3]);

    // Nothing before a hello: a console on the network that has not said it.
    F[4].up = 1;
    f_join(&F[4], 0);
    lanlobby_default_settings(&s, 0, 4001, 0);
    memset(b, 0, sizeof b);
    b[0] = 6; b[1] = 1; b[5] = 1;        // a sprint for six
    f_send(&F[4], 0, LAN_MSG_CREATE, b, sizeof b);
    frames(3, 16);
    if (L[0].numGames != 1 || (L[0].present & (1u << 4)))
        FAULT("a console made a game before its hello");

    // A client's link to the server timed out: it is told.
    clear_events();
    F[2].deaf = 1;
    frames(2, 16);
    if (!saw(2, LAN_EV_SERVER_LOST) || L[2].on)
        FAULT("console 2 not told its link went");

    // The server's network went under it.
    clear_events();
    F[0].st = WIRELESS_IDLE;
    frames(2, 16);
    if (!saw(0, LAN_EV_SERVER_LOST) || L[0].on)
        FAULT("the server not told its network went");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    printf("join six\n");        s_join_six();
    printf("password, full\n");  s_password_full();
    printf("countdown, GO\n");   s_countdown_go();
    printf("unready dropped\n"); s_unready_dropped();
    printf("late joiner\n");     s_countdown_unready_join();
    printf("creator leaves\n");  s_creator_leaves();
    printf("console lost\n");    s_console_lost();
    printf("evenly matched\n");  s_evenly_matched();
    printf("settings\n");        s_settings_clamped();
    printf("kick, busy\n");      s_kick_and_busy();
    printf("names\n");           s_names();
    printf("link\n");            s_link();
    printf("faults %d\n", faults);
    return 0;
}
