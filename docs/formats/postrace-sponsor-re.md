# UI_SponsorPopup / UI_Sponsorship_new -- decoded and ported

Decoded off the GameCube `Speed.elf` and ported on 2026-10-26 for TASKS.md
#220 (`main.c`: `careerNeedToAskForSponsor`, `careerChooseSponsor`,
`postRaceSponsorItems` / `Show` / `Scroll` / `Input`, the sponsor half of
`postRaceBuild`). A single race won past stage 1, while the stage has no
sponsor, sees these two after Rewards and before the menu.

## When they are on the list

`cPostRaceScreenManager`'s ctor (0x800F676C) adds them between Stats and the
menu, both behind `cPostRaceScreenManager::NeedToAskForSponsor` (0x800F652C):

1. Career (the caller's test), and no event or one of a single track.
2. `RuntimeCareerStageInfo::NeedToAskForSponsor` (0x80124940) on the current
   stage: no sponsor yet (stage info +0xE8 == 0), and at least one of the
   stage's sponsors whose world-race requirement is met
   (`GetNumWorldRacesCompleted`: the world-race slots holding an event).
3. Past stage 1.
4. `GetNumSponsorsToChooseFrom` at the *previous* stage's
   `GetAverageSponsorPointsPerRace` (0x801248F0; 0 until a race counted) is
   above 0: the sponsors whose world-race requirement is met and whose minimum
   average (+0xE) the average reaches.

The data orders a stage's sponsors by minimum average, so the n on offer are
the stage's first n -- `GetChooseableSponsorByIndex 0..n-1`.

`NotifyCommitSponsorPoints` (0x80124B08) feeds the average: each career race
adds the reputation its margin earns to the stage's points and counts one
more race -- except the prologue's two D-Day events.

## UI_SponsorPopup.fng -- uiSponsorPopup

"SELECT A SPONSOR!" over the sponsor icon, Back and Select. No state of its
own: Select steps on, Back steps back.

## UI_Sponsorship_new.fng -- uiSponsorsMain

`Setup` (0x800FA0C0): a `SponsorOption` per sponsor on offer, its icon
`FEHashUpper("DECAL_ICON_<name>")`, then `SetInitialPos(1)` -- 1-based
(0x801591A0 takes node r4 - 1), so the list opens on the first sponsor.

`RefreshHeader` (0x800F9EB4), for the sponsor the strip is on:

| Object | Hash | Filled with |
| --- | --- | --- |
| `SPONSOR_<NAME>` | 0xF13F76B4 | the name, language `SPONSOR_<key>`; authored "Career Status" |
| bonus | 0xF25B6BBA | `%d`, the signing bonus (sponsor +0xC) |
| URL purse | 0x7D8E1791 | `%d`, `GetURLPurse(stage)` |
| race purse | 0x195D8D22 | `%d`, the sponsor race purse (sponsor +2) |
| info | 0xF13CEF3F | `SetLanguageHash(SPONSOR_INFO_<key>)`, the required races |

It also narrows the IconScroller to 120 (+0xE4), so `ClipEdges` leaves only
the sponsor that is up.

The terms (`SPONSOR_DETAILS_<key>`) go through a `CTextScroller`: `SetText`
splits at the text's breaks, word-wraps at `TERMS_1`'s width and puts the top
back to 0; `Display` writes line top + k into `TERMS_<k+1>`, "" past the end.
The FEScrollBar "SideScroll" beside them is vertical and not resized:

| Object | Hash |
| --- | --- |
| `SideScroll_Backing` | 0x5BA12CE1 |
| `SideScroll_Handle` | 0xF985DFDE |
| `SideScroll_Arrow_1` (top) | 0x3760818D |
| `SideScroll_Arrow_2` | 0x3760818E |

### The pad

- LEFT / RIGHT move the strip; a new sponsor is `RefreshHeader`'s.
- UP / DOWN scroll the `CTextScroller`.
- Back (0x911AB364) plays FORWARD on the event handler and steps back as it
  ends.
- A is `SponsorOption::React` (0x80309B00): only while the stage has no
  sponsor, `ShowTwoButtons` on `IG_GenericDialog_SMALL` with "Want to sign on
  with %s?" (0x39DF9562) of the sponsor's name. Yes posts 0xD05FC3A3, No
  0x34DC1BCF, and Back posts No.

`uiSponsorsMain::NotificationMessage` (0x800F9CC8): Yes signs the sponsor and
steps on; No leaves the screen as it is.

## Signing

`NotifyChooseSponsor` (0x80125E80), only while the stage has none:

1. The first locked career slot opens (`UnlockACareerCarSlot`, 0x80134048).
2. The signing bonus goes into the bank.
3. The sponsor is the stage's (+0xE8), which `GetEventPurse` pays a sponsor
   race from.

## Back past them

The menu's Back (0x800F2ED4) calls `NeedToRemoveTheSponsorScreens` first:
career, and the stage now has its sponsor. If so the pair comes off the list,
so Back steps over them rather than offering a second signing.

## Port notes

- Both screens draw on the top screen, over the street, as the pause menu
  does (`worldUiOnBottom` excludes the `UI_Sponsor` prefix), and the confirm
  draws over them the way the pause menu's does.
- The right-hand column's values sit close to their labels at the 3DS
  legibility floor. `postRaceSponsorFitLabels` gives each label the width up
  to its value (the TASKS.md #129 cell rule), so a label that would overrun
  shrinks instead.
- `DbgSponsorAsk=1` forgets the stage's sponsor in memory, so a test race
  can see the pair on a career that already signed one.
