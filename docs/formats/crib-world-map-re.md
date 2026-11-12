# CribWorldMapScreen (`UI_CareerWorldMap.fng`) — decoded and ported

Decoded 2026-10-25 for TASKS.md #259 item 7 and ported the same day: the model is
`3DS Files/source/cribmap.[ch]`, the widgets, strings, map texture, pad, touch and the race
start are in `main.c` (search `isWorldMap`). What the port does, and where it chose between PC
and GC, is under "Port" at the end.

Sources: PC `SPEED2.EXE` (addresses below are PC unless marked GC) and GC `Speed.elf`
(symbols). The raw Ghidra output is in `docs/formats/crib-world-map/`:

| File | Holds |
|---|---|
| `cribmap.c` | GC ctor 0x800D8FA0, buildRegions 0x800D9198, HasFinishedStage 0x800D946C |
| `cribmap2.c` | GC SetupRegionEvents 0x800D97C0, WorldMapWrapper ctor 0x800D671C / Tick 0x800D6D44 |
| `pc_cwm.c` | 0x4E0C30, SetupRegionEvents 0x4E0CE0, 0x4EE690 |
| `pc_cwm2.c` | NotificationMessage 0x4E0F60, 0x4E13A0 |
| `pc_cwm3.c` | PlayableEvent ctor 0x4B1200, TrackMapScroller 0x4CDD40, 0x498600 |
| `pc_cwm4.c` | PlayableRegion FillScreen 0x496EF0, ZoomIn 0x498630 |
| `pc_thumb.c`, `pc_thumb2.c` | ThumbnailScroller: 0x5435D0, MoveObjectsTo 0x52ED80, SnapToHead 0x538740, 0x52EEA0, SplineSeek 0x574E30, 0x538700 |
| `pc_sbar.c` | FEScrollBar: 0x542AC0, 0x52E200, handle placement 0x52E320 |
| `pc_best.c` | best time 0x502AA0, best score 0x502C50, reversed 0x4FFDC0, timer-active 0x401820 |
| `pc_tm.c` | countdown 0x577310, start 0x5325E0, NotifyEnterEvent 0x541160 |
| `pc_stageinfo.c` | 0x500FA0 |
| `cwm_tree.txt`, `cwm_layout.txt` | the .fng widget tree and every widget's resolved canvas position |

Ghidra could not find 0x4B1990 or 0x496D70; their behaviour comes from the GC decode.
Regenerate any of these with
`powershell -ExecutionPolicy Bypass -File tools\re\ghidra_decomp.ps1 -Addrs "0x..." -Program SPEED2.EXE -Out <file>`.

## Widgets (hashes are FEHashUpper / bStringHash)

| Widget | Hash | Canvas (+Y down) |
|---|---|---|
| MINI_MASTER (icon strip origin) | 0x38E39AD7 | (-264, 24), 16x16 |
| EVENT_ICONS (70 pooled images, parked off-screen) | 0xA334A7BC | — |
| Icon texture U2_MINI_ICON | 0x0B114ADA | — |
| Track map image | 0x90FE80F3 | (-91, 22), 309x309; frame 0xFFC8509E |
| Region text (String) | 0xC8E97C9C | (0, -162) |
| Region backing "ScrollBar_Backing" | 0x688A1551 | (-1, -161) |
| Region arrows ScrollBar_Arrow_1 / _2 | 0x444969FD / 0x444969FE | |
| Region ScrollBar_Handle | 0x3FBB7A4E | |
| Highlight frames around the current icon | 0xD82B4A08 / 0xDA3C4771 | 32x32 at (-264, 24) |
| SideScrollBar_Backing | 0x884FBA76 | 8x290 at (-286, 22) |
| SideScrollBar_Handle | 0xF31E8ED3 | |
| SideScrollBar_Arrow_1 / _2 | 0x640F0F22 / 0x640F0F23 | |
| SideScrollBar_Backing_End_1 / _2 | 0x75B1DCBC / 0x75B1DCBD | |
| Info labels (rows 0..3) | 0x4D2F5C61..0x4D2F5C64 | right side, x 80..270, y -85..152 |
| Info values (rows 0..3) | 0x23EAE8B9..0x23EAE8BC | |
| Label 4 (region comment) | 0x4D2F5C65 | absent from the package, so nothing shows |
| "Event Handler" String | 0x47FF4E7C | |

The "event arrows" are the side scroll bar's own arrows.

## Data

**Regions — buildRegions (0x4CBF80).** For i = 1 upwards while HasCompletedStage(i), add a
datum to the region String with language 0x0A96B270 ("Stage %d"-style) formatted with i.
The initial region byte is 1; datum index = region - 1.

HasCompletedStage(s) reads the save: RuntimeCareerStageInfo at payload 0x993E, stride 0x101,
completed byte at +0xFC, stage number s32 at +0xFD. (See `savefile.md`.) The user's save
"Player" has stages 0, 1 and 2 completed and 3..7 not, so its regions are Stage 1 and Stage 2.

**SetupRegionEvents (0x4E0CE0).** ClearEvents; add the region item (PlayableRegion) first,
then the BuildEventList events. Each is a ThumbnailScroller node with pulse byte 1 and texture
U2_MINI_ICON; count++. Then SnapToHead, SetVisibility(1), Update, RefreshHeader. The region
node is node index 1 (1-based, 0x51F7E0).

**BuildEventList (GC; PC 0x4B1360).** Only if HasCompletedStage(region). Keep an event when:
its stage (+0x37) == region, or region == 1 and stage == 0; trackdb_find(track +0x18) exists;
and its id is not 0xDD60E402 / 0xDD60E403. Stop once more than 0x46 (70) have been added.

**PlayableEvent** fields: +4 type (event +0xF); +0x1C track; +0x20 HasCompletedEvent (state
record == 1); +8 best score 0x502C50(track, kind) for type 5; +0xC best time
0x502AA0(track, kind) otherwise; +0x10 hash of "LANGUAGE_QR_TRACK_%d_COMMENT"; +0x14
distance (+0x98); +0x18 stage. kind = 0x4FFDC0 = (event track entry +2 flags & 1), i.e.
reversed.

**PlayableEvent ctor (0x4B1200)** builds a SelectableTrack: +0x14 trackId; +0x18 zoom =
TrackInfo +0x120; +0x1C snap = TrackInfo +0x124 != 0; +0x20/+0x24 centre = TrackInfo
+0x118/+0x11C; +0x28 distance; +0x10 kind; +0x2C TrackInfo*. No TrackInfo -> zoom 512.0.
TrackInfo +0x118..+0x124 are floats centreX, centreY, zoom, then a flag byte. Examples: 4011
(131, 202, 256, 0); 4063 (84, 1, 171, 0); 4101 (159, 204, 259, 0); 4000 / 4201 / 4301 all 0.
`trackdb.c` does not read these yet (plan: `TR_OFF_SEL_CENTRE 0x118`, `TR_OFF_SEL_ZOOM 0x120`,
`TR_OFF_SEL_SNAP 0x124`).

**PlayableRegion (0x4E0B50).** StageMapInfoTable at 0x7F4388, stride 0x14: centreX, centreY,
zoom, trackId, flag. Comment = "UI_CRIB_REGION_%d_COMMENT".

| Region | Centre | Zoom | Track |
|---|---|---|---|
| 0 | (0, 0) | 512 | 4000 |
| 1 | (163, 154) | 350 | 4500 |
| 2 | (126, 93) | 387 | 4501 |
| 3 | (17, 5) | 480 | 4502 |
| 4 | (0, 0) | 512 | 4503 |
| 5 | (0, 0) | 512 | 4504 |

PlayableRegion::FillScreen (0x496EF0) blanks the five labels and values and sets label 4 to
the comment (absent widget, so the panel is empty on the region node).

**PlayableEvent::FillScreen (GC decode):**

| Row | Label | Value |
|---|---|---|
| 0 | 0xED8CA191 "Event Status" | 0xB1EE4CA9 "Completed" / 0xD11ECD79 "Not Completed" |
| 1 | 0x7F904AE8 "Race Type:" | GetRaceTypeHash(type) (`trackdb_race_type_label_hash`) |
| 2 | 0x7019ADD7 "Distance:" | "%.2f %s", distance/1000 (x0.62 imperial); 0xB5D640B1 km / 0xAC3A4F4C mi |
| 3 | type 5: 0x4E0A36A0 "Best Score:"; type 1/4: 0xDB0C5EFD "Best Time:"; else 0x3B4A1ECB "Best Lap:" | "%d" score, or the time |

**Best time / score.** HighScoresDatabase at save payload 0xC731 (0xA50 bytes); its first 320
records are 8 bytes {s16 trackId; u8 category (the lookups ignore it); u8 kind; s32 value}.
0x502AA0 returns the minimum value over records matching (track, kind); with none it falls
back to the TrackInfo par (the existing `trackdb_best_time_ticks`), or 0x7FFFFFFF. 0x502C50
returns the maximum; 0 falls back to the par score at TrackInfo +0xA4 (kind 0) / +0xA8
(kind 1). The user's save holds 37 records (e.g. track 4101, category 1, kind 0 = 516520).

## The icon strip — ThumbnailScroller, vertical (+0x30 = 0)

- AddNode: node.y = running offset; offset += node height (16) + spacing (+0x60 = 20), so the
  stride is 36 canvas units.
- MoveObjectsTo(pos) (0x52ED80): node centre = (masterX, pos + node.y + masterY) with
  MINI_MASTER at (-264, 24). Visible only when visibility byte +0x31 is set and
  |y - masterY| <= 273 x 0.5 = 136.5 -- the current node at y 24, about three either side.
  The current node draws full size, others 0.8x (+0x2E = 0), centred (alignment 1).
- Fade byte +0x2F set: alpha = node alpha x thumbnail_scale(y, centreY, 273, 0.8 x h), where
  thumbnail_scale(v, c, ext, ramp) is 0 outside [c - ext/2, c + ext/2], (v - lo)/ramp within
  ramp of the low edge, (hi - v)/ramp within ramp of the high edge, else 1. The ctor sets
  +0x2F = 0; StartDim sets +0x2F = 0 and SetVisibility(0); StopDim sets +0x2F = 1 and
  SetVisibility(1).
- Scroll (0x5437C0): returns at the list ends; otherwise spline target = -node.y, state 2.
- SplineSeek (0x574E30): cubic Hermite over 0.3 s, end velocity 0. State 2 re-inits from the
  current pos/vel: D = p1 - p0; a = v1 + v0 - 2D; b = 3D - v1 - 2v0; c = v0; d = p0;
  t += dt/0.3; pos = ((a t + b) t + c) t + d; vel = (3a t + 2b) t + c; t >= 1 snaps.
- SnapToHead (0x538740 -> 0x5386A0): current = head, spline snapped, MoveObjectsTo.
- Update (0x538760) pulse: phase +0x3C wraps above 2. Only while the current node is settled
  (-3 < y < 3) and its pulse byte +0x21 is set: f = phase <= 1 ? phase : 2 - phase;
  v = min(f x alpha x 1.1 + 127.5, 255); scale = v/8500 + 0.97 (about 0.985..1.0);
  size = scale x (w, h); phase += 0.03 per frame.

## Scroll bars

**FEScrollBar SideScrollBar** ("%s%s" + suffix). Update(1, count, index) (0x538080): count <=
index or count == 0 hides the handle and both arrows (+0x44, +0x48, +0x4C). Otherwise: backing
shown; handle shown only if index != 0; both arrows and both ends shown; then 0x52E320
(vertical, flag byte 1 = 0) places the handle: i = index - 1, step = (len - handleLen) /
max(count - 1, 1); i == 0 -> pos = handleLen x 0.5 + start; i == count - 1 -> pos = len +
start - handleLen x 0.5; else pos = handleLen x 0.5 + step x i + start; handle centre y = pos.

**RefreshHeader:** SideScrollBar.Update(1, eventCount, nodeIndex1Based). Region arrow +0x54
(0x444969FE) hidden when the Scrollerina's current == tail, +0x50 (0x444969FD) when current ==
head. Arrow +0x58 (0x640F0F23) hidden when node == tail, +0x5C (0x640F0F22) when node == head.

## The track map — TrackMapScroller (0x4CDD40)

- Loads `TRACKS\TRACKMAP%d.bin` asynchronously. On load: show, texture "TRACKMAP%d",
  InitZoom(1.0); if snap -> ZoomIn(1).
- Each tick while loaded == wanted: snap byte -> ZoomIn(1) and clear; else the countdown
  (0x577310 subtracts the frame's ticks, true while > 0); while it runs, f = timer x 0.00025;
  f <= 0.5 -> ZoomIn(1 - f/0.5).
- InitZoom (0x4B4B80): zoom == 0 -> ZoomIn(0); snap -> the snap byte; else timer =
  ftol(1.0 x 4000) ticks and ZoomIn(0). Net effect: hold 0.5 s, then zoom in over 0.5 s.
- ZoomIn(t) (0x498630): s = sin(t x pi/2); c = centre x s; z' = 512 - (512 - zoom) x s;
  UV top-left = c/512, bottom-right = (c + z')/512.
- SetTrackMap(st): wanted = TrackInfo +0x8A, or st+0x14 with no TrackInfo.
- Port plan: crop a copy of the `TrackMap` subtex's UVs from its saved base; consider loading
  at 512 (TRACKMAP_FULL_DIM) so the zoom stays sharp.

## Flow — NotificationMessage (0x4E0F60)

| Message | Action |
|---|---|
| 0x72619778 | ScrollEvent(0), up |
| 0x911C0A4B | ScrollEvent(1), down |
| 0x9120409E | ScrollRegion(0) |
| 0xB5971BF1 | ScrollRegion(1) |
| Back 0x911AB364 | ignored on the construction tick; else sound 10, back-flag, StartDim |
| Accept 0x406415E3 / 0x0C407210 | ignored on the construction tick or on node 1 (the region); else sound 8, ShowTwoButtons("", Yes 0xF770 -> 0x51123592, No 0x061C -> 0xB4623F67, cancel 0xB4623F67, opens on button 1, text 0xEC9488BB "Would you like to race this event?"), StartDim, script 0x9E99 on 0x47FF4E7C |
| 0x51123592 | start-flag = 1, then as 0xB4623F67 |
| 0xB4623F67 | StopDim, script 0x3D8EABC |

**ScrollEvent (0x4CC140):** at the ends, return. Otherwise Scroll(-1/+1), sound 6 (up) / 7
(down). If the node changed: FillScreen(labels +0x64, values +0x78), timer = now. Then
ThumbnailScroller Update, RefreshHeader, help flags.

**ScrollRegion (0x4E0E50):** at the Scrollerina head/tail, return. Otherwise region -1/+1,
Scrollerina prev/next, sound 4/5. If it changed: timer = now, SetupRegionEvents, FillScreen.
Then Scrollerina Update, RefreshHeader.

**Tick 0xC98356BA:** Scrollerina Update and ThumbnailScroller Update. Without the start-flag:
back-flag -> clear it, SwitchPackage("UI_CareerCrib.fng"); else if the selection timer +0x22C
is active (not 0, not 0x7FFFFFFF) and (now - timer) x 0.00025 >= 0.3 s -> SetTrackMap(node
+0x30), timer = 0, RefreshHeader; else TrackMapScroller.Update (skipped while the timer
runs). Then RefreshHeader. With the start-flag: ev = node +0x34; null -> as Back. Otherwise
DeallocBackupDB (the port's `cribDropSnapshot()`); NotifyEnterEvent 0x541160 (+0x872C++,
+0x8730 = ev, the event's state 2 unless already 1; the spawn is saved only if a player car
exists, which it does not in the frontend); 0x5325E0 (a global = 9, StartCareerEvent(ev, 1));
UnloadFrontend.

Constants used above: 0.3 s settle (0x78758C), 0.00025 ticks->s (0x784268), 1.1
(0x787598), 127.5 (0x79B954), 255 (0x784544), 1/8500 (0x79B950), 0.97 (0x79B94C), 0.03
(0x788150), 0.8 (0x7850EC), pi/2 (0x784254), 512 (0x78A18C), 1/512 (0x78F8CC).

## Port

**Data.** `profile_save.[ch]` reads the stage-completed bytes (`OFF_STAGEINFO 0x00993E`) and
the high-score records (`OFF_HIGHSCORES 0x00C731`; `profile_best_time()` /
`profile_best_score()` are 0x502AA0 / 0x502C50 without the par fallback, which
`cribmap.c` adds). `trackdb.[ch]` reads `selCentreX/Y`, `selZoom` and `selSnap` from TrackInfo
+0x118..+0x124. StageMapInfoTable is copied into `cribmap.c`.

**Model (`cribmap.c`).** buildRegions, SetupRegionEvents / BuildEventList, Scroll,
SplineSeek, MoveObjectsTo with thumbnail_scale, SnapToHead, the 0.3 s settle then
SetTrackMap, TrackMapScroller's countdown and InitZoom / ZoomIn, the side handle (0x52E320)
and RefreshHeader's arrows. `cribmap_select_node()` is the port's own: Scroll's steps repeated
until the tapped node is current.

**Where PC and GC differ, and what the port does:**

| Point | PC | GC | Port |
|---|---|---|---|
| Event cap in BuildEventList | stops after more than 0x46 (71 nodes with the region) | 61 | PC, 71 |
| Node alpha with the fade byte set | 255 x s | s³ x 255 | PC |
| Node outside MoveObjectsTo's window | colour zeroed as well as hidden | only hidden, colour kept | GC |

Both alpha formulas give 255 in the middle of the window and differ only on the ramp at its
two ends. Until the first StopDim the fade byte is clear, so every node keeps the EVENT_ICONS
widgets' authored alpha (180).

**Found while porting:**

- **The pulse never fires.** Update's pulse test is an absolute `-3 < y < 3` on the current
  node's drawn y, but a settled node sits at MINI_MASTER's y, 24. The current node stays at
  16x16 and the highlight frames do not pulse. The port has no pulse.
- **MINI_MASTER is hidden by the scroller's ctor.** It is only the strip's origin, and the
  port does not draw it.
- **The region bar's handle never shows.** SetPosResized keeps its y at -414, off the canvas.
- **FEScrollBar::Update shows the side handle only when count > 1.**
- **Group 0x7B5893B5 is turned 90°** (quaternion (0, 0, .7071, .7071)). Inside it
  is a 272x28 `U2_Menu_Box_White` bar at x 76, with a 28x28 end cap on each side (x -74 and
  226; the left one half-turned). Turned, they make the rounded vertical frame behind the
  icon strip, centred at (-265, 22). Before the port turned
  groups (`fngGroupTurns()` / `foldGroupTurns()` in main.c, which rotate children's positions
  and compose into their own rotation), it drew as a white bar across the map. A scan of
  every `extracted/fng` file found 13 turned groups. 0x7B5893B4 is the same 90° frame on eight
  garage screens, and Credits / 2P_PressStart carry half turns. `tools/fng_layout.py`'s
  dump does not apply group rotation, so `cwm_layout.txt` still shows the frame unturned.
- **Race type 3 has no English string.** GetRaceTypeHash (Speed.elf 0x800D8B28) in EventType
  order is 0x268D063C Circuit, 0xBA613729 Sprint, 0x0FDA6758 Street X, "Untitled Race" (absent
  from English.bin), 0x40C91BA7 Drag, 0x59ECB2C2 Drift. Type 3 is the DDAY pair
  BuildEventList drops, so it never shows.
- **"Best Lap: 0.00" on some unraced events is the formatter's.** `formatEngineTime()`
  (retail's) prints 0.00 for a time of zero or 0x7FFFFFFF. On the user's save,
  STAGE_0_CIRCUIT_0 (4011) and STAGE_0_SPRINT_0 (4102) show it. The save has no kind-0
  record for either (4102's one record is kind 1, 111.53 s), so 0x502AA0 falls back to the
  TrackInfo par, which is zero.

**Yes.** `cribDropSnapshot()`; the event's state becomes 2 unless it is already 1
(NotifyEnterEvent's write); the event is copied to `g_mapEvent`, and the crib enters the
world. `trackGridSpawn()` (Quick Race's `qrGridSpawn()`, generalised) puts the car on grid
position 1 of the event's first track. `mapRaceBegin()` then finds the event in `g_shops` by id
and starts it through `raceStartTrack(0)`. The post-race spawn is the profile respawn
(StartCareerEvent leaves the player's saved spawn alone), and `careerKeepSpawn()` is not
called. SetTutorialCompleted and the Help flag (+0x22A) are not ported.

**Touch (#265, the user's model of 2026-10-24).** The shared helpers are `touchUpdate()`,
`touchTapped()` and `menuTouchHitsWidget()` in main.c. On this screen (`wmTouch()`):

- Tap a region arrow -> ScrollRegion.
- Tap a side arrow -> ScrollEvent.
- Tap a strip node -> it becomes current. Tapping the current node again is A.
- Drag in the strip's column -> one ScrollEvent per stride.

There is no touch back. Retail's PC mouse model is close to this: click the region arrows,
wheel for ScrollEvent, click MINI_MASTER (the current node) to accept.
`tools/run_azahar_test_career.ps1` posts `tap:x:y` / `drag:x0:y0:x1:y1` to Azahar's render
window as mouse messages, with no focus change.

**Verified on Azahar, 2026-10-25, on the user's career save:**

- The pad: both regions, every node, the per-node maps and zooms, and accept on the region
  node ignored.
- Touch: tap select, tap again opening the question, drags of three nodes and two, and the
  region and side arrows. The harness logged no foreground change.
- The question opens on No; No restores the strip.
- Yes on STAGE_0_CIRCUIT_0 put the car on track 4011's grid with the countdown, two laps and
  opponents.
