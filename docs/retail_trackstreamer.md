# Retail TrackStreamer (Speed.elf), decoded for #270

How the GameCube game decides which world sections to load, in what order,
and what it throws out. Decompiled 2026-10-25 with
`tools/re/ghidra_decomp.ps1` (SpeedPS.elf for the four functions with
paired-singles in them) and the `DecompileNames.java` recipe on Speed.elf for
the rest. Constants read straight out of `Gamecube Files/Speed.elf`.

The port's pager (`world.c`, `world_update()`) is still a stand-in: the
nearest missing section inside `LOAD_RADIUS` 260 m, on the visible list or
not, dropped past `DROP_RADIUS` 420 m, at most `WORLD_MAX_RESIDENT` 20. This
file is what to replace it with.

## The data it runs on

L4RA.BUN's `0x80034150` group (the VisibleSectionManager):

| chunk | what |
|---|---|
| `0x34151` | 808 B: u32 20, u32 259, then 400 u16 section numbers (A01..A19, B01..B07, C01..) -- not yet used |
| `0x34152` | boundaries: outline, bbox and centre per section (the port reads it) |
| `0x34153` | 255 drivable sections, 164 B each: number, visible count, visible[72] (the port reads it) |
| `0x34154` | TrackSpecificSections, 22 x 32 B: +8 track number, +0xC count, +0x10 u16 sections. Only tracks 4001-4012 and 4101-4110, each naming one Z section (Z01..Z12); the port has no Z files |
| `0x34155` | 8132 B of named records ("CC1", "CC10", ...) with section numbers -- probably the LoadingSections `FindLoadingSection` returns; not decoded |

Section numbers are letter x 100 + n: X = 24xx, Y = 25xx, Z = 26xx.

## Two streaming positions

`StreamingPositionEntry`, 0x5C bytes, two of them at TrackStreamer+0xB0
(player 1 and 2). Fields used here: +0x00 car index (-1 = none), +0x04 pos
(bVector2), +0x0C velocity, +0x14 "valid", +0x18 current zone (short), +0x1A
pending zone, +0x1C frames the pending zone has held.

- `SetStreamingPosition(i, pos)` 0x80205924: pos, zero velocity, forces a
  zone re-check (+0x290 = 1). Called by the NIS intro/end/shop code.
- `PredictStreamingPosition(i, pos, vel)` 0x80205994: pos and velocity.
  Called every frame from `UpdateCameraMovers` 0x800765CC.

## Which zone: GetPredictedZone 0x802059D4

With a car attached, pos and velocity come from the car (+0x58/+0x5C,
+0x68/+0x6C). Then, for every TrackPathManager zone of type 12 that contains
the car:

- speed <= 178.8 m/s: the predicted point is pos + vel x 1.5 s, but never
  more than 100 m ahead (`speed x 1.5 > 100` -> pos + vel x 100/speed);
- the drivable section at that point is taken only if it is one of the
  zone's (up to 4) allowed sections, and only if the line from the car to the
  point crosses none of the streamer's barrier lines.

Outside such zones -- or when nothing qualifies -- the zone is simply the
drivable section the car is in (`FindDrivableSection(pos)`). So retail looks
ahead only where the designers placed a type-12 zone; elsewhere it streams the
visible list of where the car is.

`DetermineCurrentZones` 0x802066DC adds hysteresis (+0x1C counts frames the
new answer has held) and `HandleZoneSwitching` calls `SwitchZones` when
either position's zone changed.

## What to load: DetermineStreamingSections 0x80205DF8

The list is rebuilt from scratch on a zone switch:

1. Always: sections 2500 (Y00), 2400 (X00), 2600 (Z00).
2. The race's TrackSpecificSections (`0x34154`, by the current track number).
3. Up to four extra sections from TrackStreamer+0x6B8 (set elsewhere).
4. For each position: if its zone belongs to a LoadingSection,
   `GetSectionsToLoad` (up to 384); otherwise the drivable section's own
   visible list.

`AddCurrentStreamingSections` 0x80205CE4 stamps each listed section's +0x34
with the current frame counter (its LRU time) and sets the position's bit in
+0x0B ("wanted by position i").

## Unloading: keep it until the memory is needed

`SwitchZones` 0x80206A6C deactivates (state 4 -> 3) every active section
nobody wants any more, except X and Y. State 3 is *loaded but not active*:
the bytes stay. `UnloadLeastRecentlyUsedSection` 0x80205068 frees the state-3
section with the oldest +0x34, and `MakeSpaceInPool` 0x802065E4 calls it only
when the largest free block is too small for the next load (then hole filling,
then `JettisonLeastImportantSection`). So a section you drive away from and
come back to is usually still there. The port drops at 420 m instead, which
is why A27 was read twice in one race.

`ChooseSectionToJettison` 0x80205234 (memory emergency only): X/Y sections
score 2 (X00/Y00 1), lettered A..T sections 10, or 999 if their number's
n >= 2 x the LOD offset (the low-detail copies); +1 when not active.

## In what order: AssignLoadingPriority 0x8020714C

For every wanted section, the lowest `GetLoadingPriority` over the positions
that want it, then `priority = bucket x 100000 + section[+0x20]`.
`StartLoadingSections` 0x80207208 loads the lowest first (a disc bundle gets
-1, first of all), at most two requests in flight.

`GetLoadingPriority` 0x80206E74, per section and position:

- 0 when the section has no radius (+0x2C == 0) or the car is slower than
  1 m/s;
- p = pos + vel x 1 s; d = the distance from p to outside the section's
  boundary (`GetDistanceOutside`, 0 inside, 999 cap);
- f = min(speed / 60, 1);
- a = the angle in degrees between the velocity and p -> section centre
  (+0x24/+0x28), clamped to 20..90;
- bucket = int(d x (1 - (90 - a) / 70 x f x 0.67) / 75), clamped 0..2.

So straight ahead at 60 m/s or more a section counts at a third of its
distance, beside or behind at its full distance, in 75 m steps. +0x20, the
tie-break inside a bucket, is not decoded yet; on a disc game it is most
likely the file order.

`GetTimeToSection` 0x802073E0: seconds until the car reaches the section's
boundary, from pos and pos + vel x 0.01, 99 if it is not closing. Only used
by `GetTimeToClosestSection` 0x8020753C, for sections waiting on game-state
activation.

## Constants

| address | value | use |
|---|---|---|
| 0x80338590 | 178.811 | top speed for prediction (m/s) |
| 0x80338594 | 1.5 | look-ahead (s) |
| 0x80338598 | 100 | look-ahead cap (m) |
| 0x8033864C | 60 | speed for the full straight-ahead discount (m/s) |
| 0x8033865C / 0x80338660 | 20 / 90 | angle clamp (degrees) |
| 0x80338664 | 70 | angle range (90 - 20) |
| 0x80338668 | 0.67 | largest discount |
| 0x8033866C | 75 | bucket size (m) |
| 0x80338648 / 0x803386A8 | 999 | GetDistanceOutside cap |
| 0x803386A0 / 0x803386B0 | 99 | "never" for GetTimeToSection |
| 0x803386A4 | 0.01 | GetTimeToSection's step (s) |

## What the port's data asks of it (L4RA, measured 2026-10-25)

- Visible lists: geometry 3.7-17.5 MB (median 10.4); about 2.08x that once
  their textures come too, so a median 22 MB resident, 36 MB at worst (A6).
- Crossing into a neighbouring drivable section: 8.7 MB new (median), 17 MB
  (p90), 31 MB at worst (C12 -> B7).
- A list plus every neighbour's: 55 MB median, 68 p90, 95 worst. The N3DS
  linear heap is 128 MB.
- The textures are what make this heavier than retail's: the PICA has no
  DXT, so the bank holds RGBA8/RGB8 (4-8x the GameCube's CMPR bytes).
