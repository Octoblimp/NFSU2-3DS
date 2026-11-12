#!/usr/bin/env python3
"""Checks the LAN lobby (`3DS Files/source/lanlobby.c`, TASKS.md #321b).

One console hosts the LAN server over local wireless; the others join it,
make games and join them, and the creator starts one (the PC's flow,
docs/formats/lan-lobby-pc-re.md). tools/src/lanlobby_test.c runs six
lobbies in one process over a fake star network and checks, scenario by
scenario:

  - six consoles on one server, each welcomed with its id, every list the
    same on every console;
  - a password, wrong and missing; a full game; a second join; a game gone;
    a closed game's password slot not left to the next game;
  - the creator's start with every racer ready: a five-second countdown,
    then GO on every racer with one seed, the grid and the settings, and
    no join after it; a racer's start refused; the creator always ready
    and never kicked by itself;
  - a start with a racer not ready: dropped at once, as FUN_004bb6e0;
    too few left: refused, no one dropped;
  - a racer who joins during the countdown and is not ready, dropped at
    its end, and one GO; the grid the creator first, then by id;
  - the creator leaving closes the game; a racer leaving does not;
  - a console lost: its game closed, its place and name free, a new
    console in it is someone new; the server gone: every client told;
  - Evenly Matched against each racer's car class, and a new console in
    a place without the old one's car;
  - the settings a client sends clamped as UI_QRModeOptions' rows (laps,
    max players 2..6 in every mode, Race Breaker, Outrun's direction);
  - the creator's kick; one game a console; a game per console;
  - names cut to twelve; another program's network not listed.

    python tools/lanlobby_check.py [-v]
    python tools/lanlobby_check.py --mutate [--only NAME,...] [--jobs N]

--mutate breaks lanlobby.c one rule at a time (MUTATIONS) and checks the
harness catches each; EQUIVALENT says why the few it cannot are harmless.
"""
import argparse
import os
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostbuild                        # noqa: E402
import mutpar                           # noqa: E402

HARNESS = "tools/src/lanlobby_test.c"
LL = "3DS Files/source/lanlobby.c"
SOURCES = [HARNESS, LL]


def build_harness(tmp):
    return hostbuild.build(tmp, SOURCES, name="lanlobby_test")


def faults(lines):
    for ln in reversed(lines):
        if ln.startswith("faults "):
            return int(ln.split()[1])
    return -1


def diff(base, got, quiet=False):
    """How badly `got` fails: its faults, or 1 for a transcript with no
    tally. 0 when the harness found nothing wrong."""
    n = faults(got)
    if n == 0:
        return 0
    if not quiet:
        for ln in got:
            if ln.startswith("  FAULT"):
                print(ln)
    return n if n > 0 else 1


def M(name, before, after):
    return (name, LL, before, after)


MUTATIONS = [
    # -- names and passwords
    M("ll-name-cut", "    if (n > LAN_NAME_TYPED)\n        n = LAN_NAME_TYPED;\n", ""),
    M("ll-hello-name", "copy_name(L->name[from], nm);", "copy_name(L->name[from], \"\");"),
    M("ll-gone-name", "    memset(L->name[console], 0, LAN_NAME_MAX);\n", ""),
    M("ll-locked", "g->set.locked = L->password[L->numGames][0] != 0;",
      "g->set.locked = 0;"),
    M("ll-pw-compare", "strcmp(pw, L->password[g - L->games]) != 0",
      "strcmp(pw, L->password[0]) != 0"),
    M("ll-remove-pw", "        memcpy(L->password[k], L->password[k + 1], LAN_PASSWORD_MAX);\n",
      ""),
    # -- the wire
    M("ll-put-track", "    p[3]  = (uint8_t)(s->track >> 8);\n", "    p[3]  = 0;\n"),
    M("ll-get-track", "s->track        = (uint16_t)(p[2] | p[3] << 8);",
      "s->track        = (uint16_t)p[2];"),
    M("ll-put-mode", "    p[1]  = s->mode;\n", "    p[1]  = 0;\n"),
    M("ll-games-ready", "            g->ready     = p[3];\n", ""),
    M("ll-games-count", "        L->numGames = d[0];\n", "        L->numGames = d[0] ? 1 : 0;\n"),
    M("ll-go-seed", "    b[3] = (uint8_t)(L->go.seed >> 16 & 0xff);\n", "    b[3] = 0;\n"),
    M("ll-go-get-seed", "(uint32_t)d[3] << 16 | (uint32_t)d[4] << 24;\n        L->go.racers",
      "(uint32_t)d[4] << 24;\n        L->go.racers"),
    M("ll-go-racers", "    b[5] = (uint8_t)n;\n", "    b[5] = (uint8_t)LAN_CONSOLES;\n"),
    M("ll-names-present", "        L->present = d[0];\n", ""),
    M("ll-send-peer", "int peer = L->server ? console - 1 : 0;",
      "int peer = L->server ? console : 0;"),
    M("ll-welcome-self", "        L->self = d[0];\n", "        L->self = 1;\n"),
    M("ll-broadcast", "    if (L->dirty)\n        broadcast_lists(L);\n", ""),
    # -- settings
    M("ll-max-lo", "    if (s->maxPlayers < 2)\n        s->maxPlayers = 2;\n", ""),
    M("ll-max-hi", "    if (s->maxPlayers > LAN_CONSOLES)\n", "    if (s->maxPlayers > 8)\n"),
    M("ll-laps-lo", "    if (s->laps < lo)\n", "    if (s->laps < 1)\n"),
    M("ll-laps-hi", "    if (s->laps > hi)\n", "    if (s->laps > 10)\n"),
    M("ll-url-laps", "if (mode == 0 || mode == 4) {", "if (mode == 0) {"),
    M("ll-drift-p2p", "(mode == 3 && !pointToPoint)", "mode == 3"),
    M("ll-streetx-laps", "|| mode == 5 || mode == 10)", "|| mode == 5)"),
    M("ll-breaker", "    if (s->mode == 3 || s->mode == 10)      // FUN_004b41c0",
      "    if (s->mode == 3)      // FUN_004b41c0"),
    M("ll-outrun-dir", "    if (s->mode == 8)                       // Outrun: no direction\n"
      "        s->reverse = 0;\n", ""),
    M("ll-default-max", "    s->maxPlayers = LAN_CONSOLES;\n    s->mode",
      "    s->maxPlayers = 4;\n    s->mode"),
    # -- games
    M("ll-count-bits", "for (; m; m &= (uint8_t)(m - 1))", "for (; m; m >>= 2)"),
    M("ll-create-busy", "if (game_of(L, from) || L->numGames == LAN_MAX_GAMES) {",
      "if (L->numGames == LAN_MAX_GAMES) {"),
    M("ll-create-ready", "        g->ready   = (uint8_t)(1u << from);     // Ready_Indicator_1",
      "        g->ready   = 0;     // Ready_Indicator_1"),
    M("ll-join-gone", "        if (!g)\n            r = LAN_GONE;\n",
      "        if (!g)\n            break;\n"),
    M("ll-join-busy", "        else if (game_of(L, from))\n            r = LAN_BUSY;\n", ""),
    M("ll-join-started", "        else if (g->started)\n            r = LAN_STARTED;\n", ""),
    M("ll-join-full", "count_bits(g->members) >= g->set.maxPlayers",
      "count_bits(g->members) > g->set.maxPlayers"),
    M("ll-join-car", "L->car[from] != L->car[g->creator]", "0"),
    M("ll-join-nocar", "(!(L->carSet & (1u << from)) || ", "(0 || "),
    M("ll-join-member", "        g->members |= (uint8_t)(1u << from);\n", ""),
    M("ll-ready-creator", "if (!g || g->started || g->creator == from || n < 1)",
      "if (!g || g->started || n < 1)"),
    M("ll-unready", "            g->ready &= (uint8_t)~(1u << from);\n", ""),
    M("ll-leave-members", "        g->members &= (uint8_t)~(1u << console);\n", ""),
    M("ll-leave-ready", "        g->ready   &= (uint8_t)~(1u << console);\n", ""),
    M("ll-leave-tell", "            if (c != console && (g->members & (1u << c)))",
      "            if (g->members & (1u << c))"),
    M("ll-leave-why", "    if (why != LAN_OK)\n        reply(L, console, LAN_MSG_LEAVE, why, 0);\n", ""),
    M("ll-local-leave", "    L->inGame = 0;                              // out at once", "    // out at once"),
    M("ll-remove-shift", "        L->games[k] = L->games[k + 1];\n", ""),
    M("ll-car-gone", "    L->carSet  &= (uint8_t)~(1u << console);\n", ""),
    M("ll-gone-leave", "    server_leave_game(L, console, LAN_OK);\n    L->present", "    L->present"),
    M("ll-rehello", "        if (L->present & (1u << from))          // its place taken again\n"
      "            server_console_gone(L, from);\n", ""),
    M("ll-hello-gate", "    if (type != LAN_MSG_HELLO && !(L->present & (1u << from)))\n"
      "        return;", "    if (0)\n        return;"),
    M("ll-dead-peer", "        if (!L->net->peerAlive(L->net->ctx, i)) {",
      "        if (0) {"),
    # -- start, countdown, GO
    M("ll-start-creator", "        if (g->creator != from) {\n"
      "            reply(L, from, LAN_MSG_START, LAN_NOT_CREATOR, g->id);",
      "        if (0) {\n            reply(L, from, LAN_MSG_START, LAN_NOT_CREATOR, g->id);"),
    M("ll-start-once", "        if (L->countdownEnd[g - L->games])\n"
      "            break;                              // one runs already\n", ""),
    M("ll-start-all-ready", "if ((g->members & ~g->ready) == 0 && count_bits(g->members) >= 2) {",
      "if (count_bits(g->members) >= 2) {"),
    M("ll-start-two", "if ((g->members & ~g->ready) == 0 && count_bits(g->members) >= 2) {",
      "if ((g->members & ~g->ready) == 0) {"),
    M("ll-countdown-len", "L->countdownEnd[g - L->games] = L->now + LAN_COUNTDOWN_MS;",
      "L->countdownEnd[g - L->games] = L->now + 1000;"),
    M("ll-countdown-shown", "            if (left != g->countdown) {\n"
      "                g->countdown = left;", "            if (0) {\n"
      "                g->countdown = left;"),
    M("ll-countdown-end", "if ((int32_t)(L->now - end) >= 0) {",
      "if ((int32_t)(L->now - end) >= 3000) {"),
    M("ll-enough", "if (count_bits((uint8_t)(g->members & g->ready)) < 2) {",
      "if (count_bits((uint8_t)(g->members & g->ready)) < 1) {"),
    M("ll-enough-members", "if (count_bits((uint8_t)(g->members & g->ready)) < 2) {",
      "if (count_bits(g->members) < 2) {"),
    M("ll-drop-unready", "        if (unready & (1u << c))\n"
      "            server_leave_game(L, c, LAN_REMOVED);\n", ""),
    M("ll-go-started", "    g->started   = 1;\n    g->countdown = 0;\n",
      "    g->countdown = 0;\n"),
    M("ll-go-creator-first", "    L->go.console[n++] = g->creator;              // the creator first,\n",
      ""),
    M("ll-go-skip-creator", "        if (c != g->creator && (g->members & (1u << c)))",
      "        if (g->members & (1u << c))"),
    M("ll-go-self", "        if (L->go.console[c] == 0)\n"
      "            push_event(L, LAN_EV_GO, g->id, LAN_OK);\n        else\n", ""),
    # -- kick
    M("ll-kick-creator", "        if (g->creator != from) {\n"
      "            reply(L, from, LAN_MSG_KICK, LAN_NOT_CREATOR, g->id);",
      "        if (0) {\n            reply(L, from, LAN_MSG_KICK, LAN_NOT_CREATOR, g->id);"),
    M("ll-kick-self", "        if (c != from && (g->members & (1u << c)) && !g->started)",
      "        if ((g->members & (1u << c)) && !g->started)"),
    # -- events and the link's state
    M("ll-apply-entered", "            L->inGame = game;\n", ""),
    M("ll-apply-closed", "            push_event(L, LAN_EV_CLOSED, was, code);\n", ""),
    M("ll-client-lost", "    if (st != WIRELESS_CONNECTED || !L->net->peerAlive(L->net->ctx, 0)) {",
      "    if (st != WIRELESS_CONNECTED) {"),
    M("ll-server-lost", "        if (L->net->state(L->net->ctx) != WIRELESS_HOSTING) {",
      "        if (0) {"),
    M("ll-scan-tag", "h[i].appdata[0] != APPDATA_TAG ||", ""),
    M("ll-event-order", "    *out = L->ev[L->evHead];\n",
      "    *out = L->ev[(L->evHead + L->evCount - 1) % LAN_EVENTS];\n"),
]

EQUIVALENT = {
    "ll-leave-tell":
        "A creator told its own game closed: it is the one leaving, so its "
        "lanlobby_leave_game has already cleared inGame and apply_result "
        "drops the answer; a creator gone is not there to hear it.",
}


def run_harness(exe, out, timeout):
    return mutpar.run_exe([exe], out, timeout)


def mutate_pass(only=None, jobs=None):
    todo = [m for m in MUTATIONS if only is None or m[0] in only]
    if only:
        unknown = set(only) - {m[0] for m in MUTATIONS}
        if unknown:
            print("no such mutation: %s" % ", ".join(sorted(unknown)))
            return 1
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_harness(tmp)
        out = Path(tmp) / "base.txt"
        t0 = time.monotonic()
        rc = run_harness(exe, out, 600)
        took = time.monotonic() - t0
        base = out.read_text(encoding="latin-1").splitlines()
        if rc != 0 or "faults 0" not in base:
            print("base already fails -- fix that before mutating")
            return 1
        print("base: %d lines, 0 faults, %.1f s" % (len(base), took),
              flush=True)
    limit = mutpar.timeout_for(took)
    tally = mutpar.run_pass(
        todo, SOURCES, "lanlobby_test",
        lambda exe, out: run_harness(exe, out, limit),
        base, diff, EQUIVALENT, jobs)
    return mutpar.summarize(todo, only, EQUIVALENT, tally)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", action="store_true", help="the lobbies' log lines")
    ap.add_argument("--mutate", action="store_true")
    ap.add_argument("--only", metavar="NAME,...",
                    help="with --mutate, only these mutations")
    ap.add_argument("--jobs", type=int, metavar="N",
                    help="with --mutate, mutants at once (default %d)"
                    % mutpar.JOBS)
    args = ap.parse_args()
    if args.mutate:
        sys.exit(mutate_pass(args.only.split(",") if args.only else None,
                             args.jobs))
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_harness(tmp)
        out = Path(tmp) / "out.txt"
        argv = [exe] + (["-v"] if args.v else [])
        rc = mutpar.run_exe(argv, out, 600)
        lines = out.read_text(encoding="latin-1").splitlines()
    for ln in lines:
        print(ln)
    sys.exit(1 if rc or "faults 0" not in lines else 0)


if __name__ == "__main__":
    main()
