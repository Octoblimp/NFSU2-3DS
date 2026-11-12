# UI_PostRace_TournResults / UI_PostRace_TournStandings -- decoded and ported

Decoded off the GameCube `Speed.elf` and ported on 2026-10-26 for TASKS.md
#259 item 7 / #220 (`main.c`, `postRaceTournText` and the `tourn` half of
`postRaceBuild`). A career event of more than one track sees these two in
place of the Results screen after every track.

## When they are on the list

`cPostRaceScreenManager`'s ctor (0x800F676C) adds, in order: Results,
TournResults, TournStandings, Rewards, Stats, the two sponsor screens, Menu.

| Test | Address | True when |
| --- | --- | --- |
| NeedToSeePostRaceResults | 0x800F6268 | career: not (an event is up and it has more than one track); otherwise not the Outrun or online tourney |
| NeedToSeePostRaceTournamentResults | 0x800F6304 | `TheRaceParameters + 0x64` == 0, and: career with an event of more than one track (`DAT_803BBEC0 != 0 && DAT_803B82BC > 1`); or LAN / online settings mode 8; or Quick Race Outrun (`803CE800 == 4 && 803BC330 == 8`); or `803CE800 == 8 && 803CE820 == 1` |
| NeedToSeePostRaceTournamentStandings | 0x800F6420 | the same, without the last arm |

The Outrun and online arms have no port (no online, no Outrun mode), so the
port's test is career with `trackCount > 1`.

## The packages

Both are in `GLOBAL/InGameRace.bun`. They share Results' column hashes:

| Object | Hash | Notes |
| --- | --- | --- |
| `RANKING_DATA_n` | 0xDA2E9A32 + n-1 | has INIT / HIGHLIGHT / UNHIGHLIGHT |
| `PLAYER_DATA_n` | 0xA5CC56B5 + n-1 | |
| `CAR_DATA_n` | 0xFE58E79E + n-1 | |
| `TIME_DATA_n` | 0xA3EA9C37 + n-1 | TournResults only |
| `PLAYER_POINT_DATA_n` | 0xF2028E7E + n-1 | |
| row bars | 0x949F4946 + n-1 | Groups; INIT chains to HIDE |
| `NEXT` | 0x0019DE3E | the A legend's word, "Next" (0x93447EE3) |
| countdown | 0x9B8C9D7A | INIT chains to HIDE, online |
| server wait | 0x1F105857 | TournResults, INIT chains to HIDE, online |
| `OVERTIME` | 0xF6D3C68A | TournStandings, INIT chains to HIDE, Outrun |

Titles: TournResults' `HEADER_TEXT` is "Tournament Race Results"
(0xE59E8200); TournStandings' is "Tournament Standings" (0x7E891F93).
Headers: Name, Car, Time (TournResults' `HEADER_TEXT_3`), Points.

`RANKING_DATA_n`'s HIGHLIGHT keys itself to B,G,R 8A,F4,CC and sends an
event to each of the row's other Strings and its bar: five targets on
TournResults (name, car, time, bar, points), four on TournStandings (no
time column).

## cPostRaceTournamentResults

| Method | Address |
| --- | --- |
| ctor | 0x800F6C08 |
| NotificationMessage | 0x800F6CF0 |
| SetUnusedEntriesInvisible(6, n) | 0x800F6DDC |
| Setup | 0x800F6FD0 |
| OrganizeRaceResults | 0x800F7118 |
| PrintRaceResults | 0x800F7270 |
| SetRaceSpecificHeader | 0x800F745C |
| PrintRacePosition | 0x800F74E8 |
| PrintDriverName | 0x800F7614 |
| PrintCarName | 0x800F76E0 |
| PrintRaceTime | 0x800F7764 |
| PrintRacePoints | 0x800F7854 |

- The ctor runs Setup: SetRaceSpecificHeader (a drift event's
  `HEADER_TEXT_3` reads "Score", 0xC03F9F19), OrganizeRaceResults,
  PrintRaceResults, then SetUnusedEntriesInvisible(6, event cars), which
  shows rows 1..n and hides the rest of the five columns.
- OrganizeRaceResults: for each event car (`GetEventCarByIndex`, the
  EventInProgressState at 0x803B4DC8, count at +0x5C) a node of its
  `FinishedRaceStatsEntry` (`GetStatsForDriver`), car number,
  `GetPointsThisRace` and `GetPointsTotal`; then `bList::Sort` by
  `SortByFinishPosition` (`a.pos <= b.pos`).
- GetPointsThisRace (0x80127F44): `TournamentPointsTable[place - 1]`, the
  table at 0x8035C790: 8, 6, 4, 3, 2, 1.
- PrintRaceResults, per row: `"%2d"` (0x80324874) of the entry's position;
  `"%s"` of the driver's name (`Player::GetName` for the human, else
  `driverInfo + 0x8EC`); `CAR_NAME_%s` as the language hash;
  `PrintToString(entry + 0xC, 0)`; `"%$d"` (0x80322E68) of the points this
  race. PrintRacePosition runs HIGHLIGHT on the human's `RANKING_DATA`.
- NotificationMessage: 0x406415E3 (accept) ChangeToNextScreen, 0x911AB364
  (back) ChangeToPrevScreen -- which does nothing on the list's first
  screen -- and 0xC98356BA (every frame) PostRaceMessageCenter::Update.

## cPostRaceTournamentStandings

Derived from cPostRaceTournamentResults.

| Method | Address |
| --- | --- |
| ctor | 0x800F78E8 |
| Setup | 0x800F7938 |
| OrganizeStandings | 0x800F7BA8 |
| PrintRaceStandings | 0x800F7D80 |
| PrintOrdinalPlacing | 0x800F7F10 |
| PrintPointsTotal | 0x800F807C |
| NotificationMessage | 0x800F80D8 (as TournResults') |

- The ctor runs the TournResults ctor first, and so TournResults::Setup,
  on the Standings package; then its own Setup.
- OrganizeStandings: the same nodes in event-car order, sorted by
  `SortByPointsTotal` (`b.total <= a.total`, most first). `bList::Sort`
  moves a node back only past a strictly smaller total, so ties keep
  event-car order and the player (car 0) heads a tie -- as
  GetTournamentWinnerDriverNumber gives a tie to driver 0.
- PrintRaceStandings, per row: PrintOrdinalPlacing (`"%d"`, 0x803228E0, of
  the row number, and HIGHLIGHT on the human's row), the name, the car, and
  PrintPointsTotal (`"%$d"` of the total).
- Setup ends with `NEXT` (FEPrintf on the object named `"NEXT"`, 0x80324B0C)
  set to "Next Race" (0x23F359B8) while IsThereANextRace; Outrun's won
  tourney (0x224936F4, "Quit") has no port.

### Two highlighted rows

TournResults::Setup has already run HIGHLIGHT on the player's finish row
when Standings' own Setup runs it on the player's standings row, and nothing
runs UNHIGHLIGHT. After the first track the two rows are the same (the
points table falls with every place); from the second on they can differ,
and then both rows are lime with their bars up. The port shows both.
TournResults::Setup's other prints on this package are overwritten
(`RANKING_DATA`, the name, the car, the points) or land on Strings it does
not have (`TIME_DATA`).

## What the port does

- `postRaceBuild` puts TournResults and TournStandings on the list in place
  of Results for a career event of more than one track, and fills
  `PostRace.tourn[0]` (finish order, the track's points) and
  `PostRace.tourn[1]` (standings order, `racePointsTotal`), with
  `tournHi[]` the highlighted rows' mask.
- `postRaceTournText` prints the rows; `postRaceLayout` hides the online and
  Outrun Strings, the rows past the last car and every bar but a
  highlighted row's; `postRaceTextColor` puts the HIGHLIGHT lime on a
  highlighted row's Strings.
- A goes on, B back (nothing on TournResults, the first screen).
- Not ported: the drift header (no drift mode yet, #268), the Outrun and
  online arms, PostRaceMessageCenter.
- `DbgRaceTracks=2` with `DbgRaceNow=0` starts the first tournament the card
  can (the harness: `-RaceNow 0 -ExtraConfig "DbgRaceTracks=2"`).
