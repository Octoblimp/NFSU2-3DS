// lannet -- lanlobby's LanNet over wireless.c: see lannet.h.

#include "lannet.h"

#include <3ds.h>

#include "wireless.h"
#include "wlproto.h"

static WirelessLogFn s_log;

// The keepalive (#321b): the frontend pumps the link once a frame, but a
// screen load blocks the main thread for seconds, past WL_TIMEOUT_MS -- the
// peers then call this console gone (seen leaving a game room: the lobby's
// load, "wireless: the host timed out"). While the network is up a thread
// pumps it whenever the main thread has not for KEEP_GAP_MS. One lock over
// every wireless call here: the packets go into each peer's wlproto link,
// which the lobby reads on the main thread later.
#define KEEP_EVERY_MS 40
#define KEEP_GAP_MS   100

static RecursiveLock s_lock;
static bool s_lockInit;
static Thread s_keep;
static volatile bool s_keepRun;
static u32 s_lastPump;
static u32 s_keepPumps;

static void lock(void)
{
    if (!s_lockInit) {
        RecursiveLock_Init(&s_lock);
        s_lockInit = true;
    }
    RecursiveLock_Lock(&s_lock);
}

static void unlock(void)
{
    RecursiveLock_Unlock(&s_lock);
}

static void keepThread(void *arg)
{
    (void)arg;
    while (s_keepRun) {
        svcSleepThread((s64)KEEP_EVERY_MS * 1000000LL);
        lock();
        const u32 now = (u32)osGetTime();
        if (s_keepRun && wireless_state() != WIRELESS_OFF &&
            (int32_t)(now - s_lastPump) >= KEEP_GAP_MS) {
            wireless_pump(now);
            s_lastPump = now;
            if (s_keepPumps++ == 0 && s_log)
                s_log("lan: the link kept alive over a blocking load");
        }
        unlock();
    }
}

static void keepStart(void)
{
    if (s_keep)
        return;
    s_lastPump = (u32)osGetTime();
    s_keepPumps = 0;
    s_keepRun = true;
    // Above the main thread (and the audio stream, one above it), as
    // gpuwatch's: it sleeps all but a pump's few hundred microseconds.
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio -= 3;
    if (prio < 0x18)
        prio = 0x18;
    s_keep = threadCreate(keepThread, NULL, 8 * 1024, prio, -1, false);
    if (!s_keep) {
        s_keepRun = false;
        if (s_log)
            s_log("lan: no keepalive thread -- a long load can drop the link");
    }
}

static void keepStop(void)
{
    if (!s_keep)
        return;
    s_keepRun = false;   // not under the lock: the thread may be waiting on it
    threadJoin(s_keep, 1000000000ULL);
    threadFree(s_keep);
    s_keep = NULL;
    if (s_keepPumps && s_log)
        s_log("lan: keepalive stopped (%lu pumps over loads)", (unsigned long)s_keepPumps);
}

static int n_start(void *c)
{
    (void)c;
    lock();
    int ok = wireless_state() != WIRELESS_OFF ||
             wireless_start(s_log, (uint32_t)osGetTime());
    unlock();
    if (ok)
        keepStart();
    return ok;
}

static void n_stop(void *c)
{
    (void)c;
    keepStop();
    lock();
    wireless_stop();
    unlock();
}

static int n_host(void *c, int consoles, const void *appdata, size_t n)
{
    (void)c;
    lock();
    int r = wireless_host(consoles, appdata, n);
    s_lastPump = (u32)osGetTime();
    unlock();
    return r;
}

static int n_scan(void *c, WirelessHost *out, int max)
{
    (void)c;
    lock();
    int r = wireless_scan(out, max);
    unlock();
    return r;
}

static int n_join(void *c, int scanIndex)
{
    (void)c;
    lock();
    int r = wireless_join(scanIndex);
    s_lastPump = (u32)osGetTime();
    unlock();
    return r;
}

static void n_leave(void *c)
{
    (void)c;
    lock();
    wireless_leave();
    unlock();
}

static WirelessState n_state(void *c)
{
    (void)c;
    lock();
    WirelessState s = wireless_state();
    unlock();
    return s;
}

static int n_peer_count(void *c)
{
    (void)c;
    lock();
    int n = wireless_peer_count();
    unlock();
    return n;
}

static int n_peer_alive(void *c, int i)
{
    (void)c;
    lock();
    int a = wireless_peer_alive(i);
    unlock();
    return a;
}

static int n_send(void *c, int peer, uint8_t type, const void *data, size_t n)
{
    (void)c;
    lock();
    WlLink *l = wireless_peer_link(peer);
    int r = l && wl_link_send(l, type, data, n, 1);
    unlock();
    return r;
}

static int n_recv(void *c, int peer, WlMsg *out)
{
    (void)c;
    lock();
    WlLink *l = wireless_peer_link(peer);
    int r = l && wl_link_recv(l, out);
    unlock();
    return r;
}

static const LanNet s_net = {
    NULL, n_start, n_stop, n_host, n_scan, n_join, n_leave, n_state,
    n_peer_count, n_peer_alive, n_send, n_recv,
};

// The race's (lanrace.h): by console id. The server's console c is its
// wireless peer c - 1; a client talks to the server, console 0, alone.
static int racePeer(int console)
{
    if (wireless_is_host())
        return console >= 1 ? console - 1 : -1;
    return console == 0 ? 0 : -1;
}

static int r_send(void *c, int console, uint8_t type, const void *data, size_t n,
                  int reliable)
{
    (void)c;
    lock();
    const int peer = racePeer(console);
    WlLink *l = peer >= 0 ? wireless_peer_link(peer) : NULL;
    int r = l && wl_link_send(l, type, data, n, reliable);
    unlock();
    return r;
}

static int r_alive(void *c, int console)
{
    (void)c;
    lock();
    const int peer = racePeer(console);
    int a = peer >= 0 && peer < wireless_peer_count() && wireless_peer_alive(peer);
    unlock();
    return a;
}

static const LanRaceNet s_raceNet = { NULL, r_send, r_alive };

const LanRaceNet *lannet_race(void)
{
    return &s_raceNet;
}

const LanNet *lannet_wireless(WirelessLogFn log)
{
    s_log = log;
    return &s_net;
}

void lannet_frame(LanLobby *L, uint32_t nowMs)
{
    lock();
    if (wireless_state() != WIRELESS_OFF) {
        wireless_pump(nowMs);
        s_lastPump = nowMs;
    }
    unlock();
    lanlobby_frame(L, nowMs);
}
