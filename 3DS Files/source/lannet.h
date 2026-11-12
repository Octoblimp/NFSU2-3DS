// lannet -- lanlobby's LanNet over the console's local wireless (TASKS
// #321b): wireless.c's network calls, and each message on its peer's
// wlproto link, reliable. lanlobby.c itself has no <3ds.h>, so the host
// check runs it over a fake (tools/lanlobby_check.py).

#ifndef LANNET_H
#define LANNET_H

#include <stdint.h>

#include "lanlobby.h"
#include "lanrace.h"

// The LanNet for lanlobby_init. `log` is wireless.c's, for wireless_start.
const LanNet *lannet_wireless(WirelessLogFn log);

// lanrace's link: each message on the console's peer's wlproto link,
// reliable or not as asked.
const LanRaceNet *lannet_race(void);

// Once a frame on the LAN screens and in a LAN race: the link's packets
// in and its ticks (wireless_pump), then the lobby's frame.
void lannet_frame(LanLobby *L, uint32_t nowMs);

#endif
