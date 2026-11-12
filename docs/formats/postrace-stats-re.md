# UI_PostRaceStats -- PostRaceStatsScreen, decoded and ported

Decoded off the GameCube `Speed.elf` on 2026-10-25 for TASKS.md #259 item 7 /
#220; ported on 2026-10-26 (`main.c`, the `postRaceStats*` functions and
`prs*` helpers). The screen is reached from the Results screen's X ("Race
Stats") and from the post-race menu's Race Stats item.

## The package

`UI_PostRaceStats.fng`, in `GLOBAL/InGameRace.bun` with the other post-race
screens. Its strings:

| Object | Text | Notes |
| --- | --- | --- |
| title | "Post Race Stats" (0xC7DA078E) | |
| `RACER_DATA` (object 0xB4701445) | "01: Little Timmy - 240 SX" | placeholder (language hash 0x9588BD38, the shared "%s" stub), replaced by Setup |
| `HEAD_TEXT_2` / `HEAD_TEXT_3` | "Race Time" (0x51C3DC07) / "Position" (0x75B6CA33) | lap table headers |
| `OPTION_NAME_%d` / `OPTION_DATA_%d` | x10 each | the stat rows (right block) |
| `TIME_DATA_%d` / `POSITION_DATA_%d` | x10 each | the lap rows (left block) |
| arrows | 0x444969FD (left) / 0x444969FE (right) | `U2_Menu_Arrow_White.tga` |
| Back | 0x933DD695, `$JOY_EVENT_FENG_CANCEL$` | both Strings carry flag 0x80000000, which retail ignores in play |

The row names are FEHashUpper of the format with the 1-based row number. The
row positions do not come from the rows themselves: four master images,
`TITLE_MASTER`, `DATA_MASTER`, `TIME_MASTER` and `POSITION_MASTER`, give the
column rectangles, and each row steps down by the title string's height
(`FEStatWidget::IncrementStartPos`; the extra spacing at +0x138 is 0).

## Methods

| Method | Address |
| --- | --- |
| ctor | 0x800F50D4 |
| NotificationMessage | 0x800F520C |
| Init | 0x800F52FC |
| ManageLapResults | 0x800F5440 |
| SetLapList | 0x800F549C |
| AddLapResult | 0x800F551C |
| GetCurrentFEString | 0x800F5670 |
| ClearLapResultWidgets | 0x800F56DC |
| Setup | 0x800F57B4 |
| SetNavigationArrows | 0x800F5D2C |
| Scroll | 0x800F5F04 |

`bCameFromPostRaceMenu` (0x8046DA88) is set by `PRSwitchToStats::React`
(0x803090E4) and read by `PostRaceResultsScreen::NotificationMessage`
(0x800F4CE4).

## Messages

| Message | Meaning | Action |
| --- | --- | --- |
| 0x9120409E | left | Scroll(-1) |
| 0xB5971BF1 | right | Scroll(+1) |
| 0x911AB364 | back | `UI_PostRace.fng` if bCameFromPostRaceMenu, else `UI_PostRaceResults.fng`; sound 0xA |
| 0x406415E3 | accept | nothing |

An earlier draft of this note called 0x9120409E / 0xB5971BF1 up / down; they
are the left / right pad messages, which is also what the arrows show.

## The constructor

The screen opens on the entry whose car number is 0 (the player). Index 0
sets the arrows as for a LEFT message; any other index calls Scroll(RIGHT)
once per step from 0, so the right-scroll sound plays on open.

## Init and Setup

Init places the lap rows from `TIME_MASTER` / `POSITION_MASTER`, then calls
Setup with the racer index at +0x1A4.

Setup(i):

- The entry is `FinishedRaceStatsEntry*` from the table at 0x803CF77C
  (FEDatabase + 0x1356C), indexed by i.
- `RACER_DATA` gets `"%2d   %s - %s"`: the entry's position (+0), the driver
  name, and the localized car model. The name is `driverInfo + 0x8EC`, or
  `Player::GetName` for the human.
- Drag or point-to-point hides `HEAD_TEXT_2` / `HEAD_TEXT_3`; otherwise
  ManageLapResults fills the lap table.
- The stat rows, in order:
  - Drift: PRScore, PRBestDriftScore, PRLongestDrift, PRTopSpeed,
    PRAverageSpeed.
  - Everything else: PRFinishTime, PRTimeBehind, PRTopSpeed, PRAverageSpeed;
    then PRPerfectShifts if drag; then PRBestLapTime and PRLapsLed if neither
    drag nor point-to-point.

The race type flags are `TheRaceParameters` (0x803FCA40): +0x18
pointToPoint, +0x20 laps, +0x40 drag (also `g_tweakIsDragRace`, r13-0x6098),
+0x44 drift (also `g_tweakIsDriftRace`, r13-0x609C).

## Placement (FEStatWidget::Position)

- A label whose string has Sj & 1 (centred) is placed with its centre x at
  `pos.x + titleW / 2` and its top at `pos.y`; otherwise its top-left is
  `pos`.
- The value is always centred: centre x at `dataPos.x + dataW / 2`, top at
  `dataPos.y`.
- The master images are centre-anchored, so a column's top-left is the
  image's position minus half its size.

## The lap list

- ManageLapResults shows laps 1..min(laps, 10). It reads a page at +0x1A8
  (page 1 would be laps 11..laps), but Scroll resets the page to 0 and
  nothing sets it to 1, so the second page is never reached in retail.
- Each lap is a `PRLapResult` (vtable 0x8038C450) on `"TIME_DATA_%d"` /
  `"POSITION_DATA_%d"` (FEHashUpper, counter at +0x1A0 from 1).
- Lap time is entry + 0x1C + lap x 4; position is entry + 0xCC + (lap-1) x 4.
- `PRLapResult::Draw`: the name is FEPrintf(`"%d   %s"` at 0x80324928,
  lap number, PrintToString(time, 0)); the data is `"%d"` of the position,
  centred in the POSITION_MASTER column.

## Scroll and the arrows

Scroll clamps the index to 0..count-1, where count is
`*(r13-0x6084) + 0x24`. If the index changed: the sound
TranslateLeftRightIntoSound, ClearWidgets, ClearLapResultWidgets, page = 0,
Init. Then SetNavigationArrows.

SetNavigationArrows(message):

- Visibility: index 0 shows only the right arrow, the last index only the
  left one, anything between both. Tracks 4174..4179
  (`track - 0x104E <= 5`) hide both.
- LEFT: the left arrow runs "HIDE" at index 0, else "BLINK"; the right one
  runs "STATIC".
- RIGHT: the right arrow runs "HIDE" at the last index, else "BLINK"; the
  left one runs "STATIC".

The arrow scripts (Scrp chunks in the package):

| Script | Keys |
| --- | --- |
| INIT | white, alpha 150, 26 x 26; chains (Sc) to HIDE |
| STATIC | white, alpha 150, 26 x 26 |
| BLINK | 400 ms, colour R 0xCA G 0xF4 B 0x8A throughout: alpha 150 / 26 x 26 at 0 ms, 255 / 32 x 32 at 200 ms, 150 / 26 x 26 at 400 ms; chains to STATIC |
| HIDE | white, alpha 0, 26 x 26 |

(Kd colour keys are stored B, G, R, A; the base key is FFFFFFFF + values and
each later key is a time in ms plus deltas from the base.)

The port plays these in the image tint hook (`postRaceStatsImageTint`)
rather than through a general FEng script player.

## The stat widgets

Each Draw sets the label's language hash on string +0x30 and the value on
+0x34.

| Widget | Label | Value |
| --- | --- | --- |
| PRFinishTime | 0xF9D7F4C2 | PrintToString(entry + 0xC, 0) |
| PRTopSpeed | 0x5D304669 | entry + 0x10C, see units below, `"%$3.1f %s"` |
| PRAverageSpeed | 0x5F38E63E | entry + 0x110, same units |
| PRTimeBehind | 0xAB44ED8B | see below |
| PRPerfectShifts | 0x680AC597 | `"%$d/%$d"` (0x80323044) of entry + 0x128, entry + 0x124 |
| PRBestLapTime | 0xAF6943CB | PrintToString(entry + 0x10) |
| PRLapsLed | 0xADBFD917 | `"%d/%d"` (0x8032493C) of entry + 0xF4, laps |
| PRBestDriftScore | 0xD7D8CA79 | `"%$d"` (0x80322E68) of entry + 0x214 |
| PRLongestDrift | 0x5C17EC47 | SetTime(float entry + 0x218), PrintToString |
| PRScore | 0xE8AEBFAF | int(float entry + 0x21C), `"%$d"` |

Speed units: `FEDatabase + 0x58` (0x803BC268) == 1 is metric, x 3.6 and unit
0xE74B7E13; otherwise x 2.237 and unit 0xE74B86F8.

PRTimeBehind switches on the finish reason at entry + 8:

| Reason | Shows |
| --- | --- |
| 8 | 0x01E66364 |
| 9 | 0xB7B75185 |
| 0xB | 0x1CE129E0 |
| 0xC | 0xA7DB717F |
| 3 | 0x5D82DBA2 |
| 1 | the winner gets 0x61C9B7A2 ("--:--.--"); the rest PrintToString(time - first time, 2) |
| other | 0xD4293449 |

## FinishedRaceStatsEntry, as far as read

| Offset | Field |
| --- | --- |
| +0x00 | position |
| +0x04 | car number |
| +0x08 | finish reason |
| +0x0C | race time |
| +0x10 | best lap |
| +0x1C | lap times, u32 per lap |
| +0xCC | lap positions, u32 per lap |
| +0xF4 | laps led |
| +0x10C | top speed (m/s) |
| +0x110 | average speed (m/s) |
| +0x124 / +0x128 | perfect shifts (the two format arguments) |
| +0x140 | best lap score (drift: the most points in one lap) |
| +0x144 | drift points per lap, int, `(int)(lap score + 0.5)` |
| +0x198 | drift points so far, int per lap (the running total) |
| +0x1E8 | drift place per lap, int, laps 0..10 |
| +0x214 | best drift score |
| +0x218 | longest drift (time) |
| +0x21C | drift score |

## What the port does

- `race.c` records each racer's top and average speed, every lap's time and
  position, and laps led; `race_finished_stats` builds the table in finish
  order (`RaceStatsEntry`, race.h).
- The port fills the circuit / sprint rows (FinishTime, TimeBehind, TopSpeed,
  AverageSpeed, then BestLapTime and LapsLed for circuits), and the drift rows
  (#268 E step 3): PRScore, PRBestDriftScore, PRLongestDrift, then TopSpeed and
  AverageSpeed; the lap list stays, as retail's does.
- In a drift (#268 E step 3), FinishedRaceStatsEntry::Construct's drift
  fields come from the player's moment and lap scores (`drift_car_stats`,
  drift.c), each car's position is its DriftManager leaderboard place
  (GetLeaderBoardPos), CalculatePositionsForEachLap re-ranks every lap by
  drift points (SortRacingCarByScore: no points goes last, then the higher
  total; laps led counts first places), and cFinishedRaceStats::Construct
  sorts the table by the final total (SortResultsByDriftPoints). The Results
  screen prints the total in the time column under the drift header
  (SetRaceSpecificHeader, string 0xC03F9F19) and skips the finish / gap
  toggle; TournResults gets the same header over its race times.
- The drift drag rows (PRPerfectShifts) wait for drag (#268 D's results).
- Left / right page through the racers with the arrows as above; B goes back
  to the post-race menu or Results, the way it came.
