# PC LAN frontend flow (SPEED2.EXE RE)

Source: the PC executable SPEED2.EXE, decompiled headless with `tools/re/ghidra_ps.ps1`.
Screen names come from `tools/_refs/nfsu2-re/docs/docs.txt`. Strings come from
`tools/_refs/nfsu2-re/docs/dumps/language_english.txt`. Layouts are in `extracted/fng/*.fng`.

This doc is the reference for rebuilding the LAN flow over 3DS local wireless.
The game room itself (UI_OLGameRoom handler 0x4E4A30, host sub-screen 0x49FAA0, client
sub-screen 0x49FD20) is covered in a separate pass. Here it appears only where the flow
passes through it.

Notation:
- `FUN_xxxxxxxx` and bare hex numbers are SPEED2.EXE virtual addresses.
- "msg 0x…" is an FEng message hash.
- A string is written as hash plus English text.
- Anything marked **(guess)** is inferred, not proven from code.

---

## 0. Correction: which context value is LAN

The global `DAT_00850078` holds the frontend context.

**`0x200` is LAN, and `8` is online.** This is the opposite of what we had assumed.
Proof:

- 0x4BC7B0 sets the UI_OLMAIN header to "LAN Main" (0x03f6f2bf) only when the context is 0x200.
- The PC_OL_Lobby constructor 0x4F1150 sets "LAN Lobby" (0xe50258b1) only when the context is 0x200.
- FUN_004a0140 stops the LAN server only when the context is 0x200 and `0x8500c7` is set.
- FUN_0049da40 returns early with a LAN-specific result when the context is 0x200, before any online-only checks.

Other context values:

| Value | Meaning |
|---|---|
| 0x100 | Online car-select variant |
| 4 | Split screen |
| 2 / 0x20 | Customise paths |
| 1 | Offline Quick Race (see 0x502FF0) |

## 1. Globals used by the LAN flow

The table at 0x850080 is the "game session settings". It is filled by the create-game
screens, and a joiner receives it from the host.

| Address | Type | Meaning | Written by |
|---|---|---|---|
| 0x850078 | int | Context (0x200 = LAN) | UI_PC_LAN_ServerSelect / main menu |
| 0x850080 | u8 | Max players | Max-players option 0x4B3110 / 0x4B3190 |
| 0x850081 | u8 | Min players | Same option |
| 0x850084 | int | 0 = this console created the game, 1 = it joined | FUN_004faa00 (create), FUN_004f1620 (join) |
| 0x850088 | int | Race mode (section 7) | FUN_004b2c00 |
| 0x85008c | int | Car performance restriction (Evenly Matched / None) | ModeOptions |
| 0x850090 | int | Outrun points-to-win index | ModeOptions (Outrun only) |
| 0x850094 | int | Outrun time-limit index | ModeOptions (Outrun only) |
| 0x850098 | u8 | Ranked. Always 0 in LAN (FUN_004b41c0) | ModeOptions |
| 0x850099 | u8 | Race Breaker | ModeOptions |
| 0x85009a | u8 | Collision detection | ModeOptions |
| 0x85009b | u8 | Password protected | ModeOptions. Reset to 0 when the list is built (FUN_004b41c0) |
| 0x85009c | char[13] | Password (max 12) | ModeOptions / UI_OLPassword |
| 0x8500a9 | char[13] | Password confirm | ModeOptions / UI_OLPassword |
| 0x8500b8 | u16 | Track id | TrackSelect, or random for Outrun (0x4B2A80) |
| 0x8500ba | u8 | Laps | ModeOptions (forced to 1 for point-to-point modes) |
| 0x8500bc | u8 | Direction: 0 forward, 1 reverse | ModeOptions |
| 0x8500c7 | u8 | 1 = this console hosts the LAN server | UI_PC_LAN_ServerSelect |
| 0x8500c8 | u8 | Private room. Online only; zeroed by the ModeOptions ctor 0x4EF3F0 | — |
| 0x8500d0 | int | Network state: 0x11 = idle in lobby, 0x12 = busy or starting | FUN_005031b0 |
| 0x850080 + 0x58 / 0x6c / 0x80 | char[16] ×3 | LAN name slots (player and server) | UI_PC_LAN |

Online uses its own name slots at +0x94 / +0xa8 / +0xbc.

---

## 2. Flow chart

```
UI_Main
  └─ LAN ─> UI_PC_LAN_ServerSelect          (Host / Join; sets 0x8500c7)
              └─> UI_PC_LAN  (enter name)
                    ├─ host: StartServer(name) -> "Starting LAN Server ..."
                    │        -> connect 127.0.0.1:9900 -> UI_OLMAIN
                    ├─ join: -> UI_OLMAIN
                    └─ Back -> UI_PC_LAN_ServerSelect

UI_OLMAIN  ("LAN Main")
  ├─ Lobby       -> PC_OL_Lobby.fng
  ├─ Select Car  -> UI_OLCarSelect.fng -> (Yes [-> Manual/Auto]) -> UI_OLMAIN
  ├─ Options     -> UI_OptionsMain -> back to UI_OLMAIN
  └─ Back -> "exit LAN mode?" (Yes: disconnect, stop server if hosting) -> UI_Main

PC_OL_Lobby.fng  ("LAN Lobby")
  ├─ Create Game -> UI_QRModeSelect
  │                  ├─ Outrun -> random track ─────────────┐
  │                  └─ other  -> UI_QRTrackSelect ──────────┤
  │                                                          v
  │                                               UI_QRModeOptions
  │                                                 └─ accept -> PC_OL_GameRoom.fng (creator)
  │                  (Back from ModeSelect -> PC_OL_Lobby)
  ├─ Join Game  -> [UI_OLPassword if locked] -> "Joining game '%s'..."
  │                 ├─ ok    -> PC_OL_GameRoom.fng (joiner)
  │                 └─ error -> dialog, stay in the lobby
  ├─ Refresh    -> re-query the games list
  ├─ Filter     -> UI_OLFilters
  └─ Back       -> UI_OLMAIN

PC_OL_GameRoom.fng  (separate RE pass)
  └─ host Start: ready check -> countdown -> start session
       -> (net race state 3->4) UI_OLPreRaceStart.fng pushed over the race load
            ├─ net state 5 -> screen dismissed, race runs
            └─ connection lost -> dialog -> OK -> back to PC_OL_Lobby.fng
  race end -> results / "Starting next race in %d seconds"
           -> frontend return to PC_OL_Lobby.fng (FUN_00502ff0)
```

---

## 3. UI_PC_LAN.fng (name entry)

| Item | Address |
|---|---|
| Builder | 0x4D6C90 |
| Constructor | 0x4BE960 |
| Vtable | 0x794FE8 |
| Handler (vtable slot 2) | 0x4A2E20 |
| Setup | FUN_004a2ff0 |
| Slot 5 | 0x4BEC20. On msg 0x0c407210 it shows an InGameDialog if FUN_00503080 is true |

### Setup (FUN_004a2ff0)

1. Shows widget `PC_NAME_ENTRY`.
2. Opens text entry with `FUN_00555f80(0x5b9d88b9, 6, 0x818f0385 "NAME", 0x22729437 "Enter name:", 12, 0)`. The maximum is 12 characters.
3. Sets `Header_text` to 0x22729437 "Enter name:".
4. Fills all three name slots (0x850080 +0x58/+0x6c/+0x80) with the default 0xd9dc2f12 "PLAYER".

Two strings are never used by the code:
- The .fng has LAN_ENTER_SERVER_NAME 0xa6cc56cd "Enter Server name", but setup overwrites the header.
- LAN_ENTER_USERNAME 0x5352cd36 has no immediate reference anywhere in the exe.

### Handler 0x4A2E20

**msg 0xda5b8712 (entry confirmed).** The entered text is copied, bounded to 16 bytes, into all three slots (FUN_00503230 / FUN_00503260 / FUN_00503290). **There is a single name, used for both the player and the server.**

- **Hosting** (`0x8500c7 == 1`): FUN_005fa300(name) loads `server.dll` and calls `StartServer(name, …)` / `IsServerRunning`.
  - On failure: dialog 0xa8cd6fd3 "There was a problem trying to start the LAN server…".
  - On success: starts a timer and shows dialog 0xb13d6a9b "Starting LAN Server ...". On the tick msg 0xc98356ba it connects to `127.0.0.1` port 0x26AC (9900), then goes to UI_OLMAIN.
- **Joining**: goes to UI_OLMAIN directly. The server was already chosen in ServerSelect.

**msg 0x911ab364 / 0xc9d30688 (Back).** Goes to UI_PC_LAN_ServerSelect.

---

## 4. UI_OLMAIN.fng (LAN main)

| Item | Address |
|---|---|
| Builder | 0x4F1940 |
| Constructor | 0x4E5F80 |
| Vtable | 0x797908 |
| Handler | 0x4F19A0 |
| Item list | 0x4BC7B0 (vtable slot 5) |

- The constructor sets the header to 0xca38d55f "Online Main Menu".
- 0x4BC7B0 replaces it with 0x03f6f2bf "LAN Main" when the context is 0x200.

### Items

| Widget | Text | Activate (slot 1) | Goes to | In LAN? |
|---|---|---|---|---|
| 0x114f5968 | 0x4815fdbd News | 0x4BC5E0 | UI_OL_WebOffer | **no** |
| 0x3b1b7603 | 0x4ab6a4f8 Lobby | 0x4BC6C0 | PC_OL_Lobby.fng | yes |
| 0x9c0a19e0 | 0xc0bedf15 Select Car | 0x4BC6F0 | UI_OLCarSelect.fng (outside LAN it also sets the context to 8) | yes |
| 0xda665928 | 0x8106d26a Rankings | 0x4BC730 | UI_OLRankings | **no** |
| 0x5a5e5137 | 0xbd81116c Options | 0x4BC760 | UI_OptionsMain, returns to UI_OLMAIN | yes |

**The LAN menu is: Lobby, Select Car, Options.** 0x4BC7B0 adds News and Rankings only when the context is not 0x200.

### Handler 0x4F19A0

**msg 0x911ab364 (Back).** In LAN it opens `GenericDialog_MED` with Yes = msg 0xd05fc3a3 and No = msg 0x34dc1bcf. The text depends on whether this console hosts:
- Hosting (`0x8500c7` set): 0x309b91ba "You are hosting a LAN server. If you exit LAN mode, any users on your server will be disconnected. Continue?"
- Not hosting: 0xc2e59520 "Are you sure you want to exit LAN mode?"

Online, the dialog text is 0xd5d46fdf instead.

**msg 0xd05fc3a3 (Yes).** Calls FUN_004a0140, which:
1. disconnects;
2. stops the server through FUN_005f08f0 if this is the LAN host;
3. frees the flow objects;
4. goes to UI_Main.

**msg 0x904aca43.** In LAN this also leads to the exit path.

**msg 0xd15504ec / 0xd15c04ec.** Disconnect notifications.

**LAN connection loss.** FUN_0049da40 (string use at 0x49DA82) shows 0xc91a3fde "You have lost your connection to the LAN Server … You will be returned to the main menu".

The string 0xa2717c2a "Your connection to the LAN server has been lost." has no code reference.

---

## 5. PC_OL_Lobby.fng

| Item | Address |
|---|---|
| Builder | 0x4FA990 (shared with UI_OLLobbyRoom) |
| Constructor | 0x4F1150 |
| Vtable | 0x795FD0 |
| Handler | 0x4FCDA0 |
| Mouse handler | FUN_004faa60 (includes column-header sort) |

**Static text**
- Title: 0xce9a2532 "Game Session Lobby". In LAN the header becomes 0xe50258b1 "LAN Lobby".
- Column headers: NAME, Type, Racers, Connection, Course.
- Counter labels: 0xc44a7ee2 "Games Created:" and 0x8deece42 "Players Online:".
- The counter values go in widget 0x4059a9c (games) and 0x154d771c (players). They are refreshed on the tick from FUN_00582340.

### Buttons

There are four buttons, `UI_PC_Generic_Button_sm_1..4`. The selected index is stored at `this+0x118`.

| Index | Text | Action |
|---|---|---|
| 0 | 0xb12454ef Create Game | FUN_004faa00 |
| 1 | 0x82cdba8b Join Game | FUN_0049edd0 -> FUN_0049ea80 |
| 2 | 0x130d90d1 Refresh | FUN_004bb3f0 -> FUN_004bb0f0 |
| 3 | 0x3a36f0a8 Filter | UI_OLFilters |

- msg 0x9120409e / 0xb5971bf1 (left / right) move the button selection.
- Up / down scroll the games list.
- Back goes to UI_OLMAIN.

### Games list (FUN_004d1380)

The list shows **15 visible rows**. Per-row widgets:
- `scroll_text_%d` (name)
- `scroll_type_%d`
- `scroll_racers_%d`, written as "%d/%d"
- `scroll_ping_%d`, written as "%dms", or "--" when unknown
- `scroll_course_%d`, from the session key "TRACK"
- `High_Light_%d`
- `Lock_Icon_%d`, shown when the game is passworded (FUN_004bafd0)
- `U2_mini_icon_%d`

The type column is chosen from flag bits in `game+0x110`:

| Bit | Text |
|---|---|
| 0x02000000 | 0x613d3106 Outrun |
| 0x04000000 | 0x93fdee74 Circuit |
| 0x08000000 | 0xdcb9f861 Sprint |
| 0x10000000 | 0x35102edf Drag |
| 0x20000000 | 0xd7162cfa Drift |
| 0x40000000 | 0x30da61e1 Street X |
| 0x80000000 | 0x75f903f4 URL Race |

Flag 0x10000 marks a password-protected game (FUN_004f1620).

**Refresh (FUN_004bb0f0).** Requests the list with `GAMES_PER_UPDATE` = 0x14 (20) and the current filter.
- While waiting it shows 0x65fda335 "Retrieving updated games list".
- On error it shows 0x8173fc6b "This filter cannot be applied right now."
- The callback is FUN_0049ec60.

**Search fields.** These are three text entries: 0x28c0ab0 / 0xcb3b8831 / 0x3a0d5aa0. FUN_004bb2e0 searches the list on column 1, 2 or 5. The hints are 0xfd820bf3 "Game name", 0xfd85bcb4 "Game Type" and 0x4d8e1223 "Course name". **The lobby has no chat.** These boxes are list search only.

### Create Game (FUN_004faa00)

1. Allowed only when the network state `0x8500d0 == 0x11`.
2. Sets `0x850084 = 0` (this console created the game).
3. FUN_004ef1b0 builds the QR flow with `FUN_004e2900(prev = PC_OL_Lobby, next = PC_OL_GameRoom)`.
4. Goes to UI_QRModeSelect.fng.

Section 6 has the rest of the create path.

### Join Game

**FUN_0049ea80.** In LAN it sends msg 0x59337816 directly. Online, it first shows a warning: 0xd157c200 (unranked) or 0xa53035dd (ranked). Online-only connection-quality warnings 0x243f8e1e (unknown) and 0xa492ae15 (below average) appear when ping < 0 or > 175 ms.

**FUN_004f1620 joins the game:**
1. Sets `0x850084 = 1`.
2. If the game has flag 0x10000 (password) and no password has been entered yet, it opens **UI_OLPassword.fng**:

   | Item | Value |
   |---|---|
   | Builder | 0x4E42C0 |
   | Constructor | 0x4D1810 |
   | Vtable | 0x796014 |
   | Handler | 0x4BB4E0 |
   | Header | 0xb34ab032 "Password" |
   | Entry | 0x332c0b6f "Enter Password", 12 chars |
   | On OK | msg 0x8905384 to the lobby, which re-runs the join |

3. Copies the password to 0x85009c / 0x8500a9.
4. FUN_0058d6b0 sends the join request and shows 0xd48fba40 "Joining game '%s'...". If the request can't be sent, it shows 0xe3a9ab2a "The game session is no longer available."

**Join result callback (FUN_0049eb10).** On success it goes to **PC_OL_GameRoom.fng**. On failure it shows a dialog chosen by the 4-character code from the server, and the player stays in the lobby:

| Code | Dialog |
|---|---|
| `game` | 0x331f4f58 "This game is no longer available to join." |
| `full` | 0xd155595a "This game session is full." |
| `pass` | 0xb5eaa702 "Incorrect password" |
| `asta` | 0x1c098ddf "This game has already started." (no joining after start) |
| `invp` | 0x4ad41e75 "Your currently selected car cannot be used in this game." |
| other | 0x7aa591cc "The game you are attempting to join no longer exists" |

---

## 6. Create-game screens (QR flow reused)

FUN_004e2900 creates the flow object at `DAT_00836340`. It is a list of nodes `{screen, nextFn, prevFn}`. FUN_004b29a0 advances to the next screen and FUN_004b2a00 goes back.

| Node | next() | prev() |
|---|---|---|
| UI_QRModeSelect | 0x4B2A80 | 0x498290 |
| UI_QRTrackSelect | 0x498240 | 0x4982B0 |
| UI_QRModeOptions | 0x498220 | 0x4B2AF0 |

What each function returns in LAN (0x200):

- **0x4B2A80** (after ModeSelect)
  - If mode == 8 (Outrun): picks a random track with `FUN_0043c1c0(0x7fffffff)` -> FUN_004fef50, stores it in 0x8500b8, and returns UI_QRModeOptions.
  - Otherwise returns UI_QRTrackSelect.
- **0x498240** (after TrackSelect): returns UI_QRModeOptions.
- **0x498220** (after ModeOptions): returns **PC_OL_GameRoom.fng**. This applies to both 8 and 0x200.
- **0x4B2AF0** (back from ModeOptions): returns UI_QRModeSelect if the mode is Outrun, otherwise UI_QRTrackSelect.
- **0x4982B0** (back from TrackSelect): returns UI_QRModeSelect.
- **0x498290** (back from ModeSelect): returns PC_OL_Lobby when the context is 8. Otherwise it returns the flow's stored "prev" screen. For LAN that is PC_OL_Lobby, passed in by FUN_004ef1b0.

### UI_QRModeSelect.fng

| Item | Address |
|---|---|
| Builder | 0x4EF280 |
| Constructor | 0x4E2A50 |
| Vtable | 0x797734 |
| Handler | 0x4B2B30 |
| Item list | 0x4B2D60 |

- Title: 0x47b276ef "Race Mode Select" in LAN (0x4E2A50). Online uses 0xb6068c57.
- Help text: 0x36bec45f.
- The `EA_MESSENGER_GROUP` widget is shown online only.

Items, in list order (0x4B2D60). FUN_004b2c00 writes the mode to `0x850088` when the context is 8 or 0x200, and to 0x83aab4 offline.

| Order | Text | Item vtable | Mode value | LAN? |
|---|---|---|---|---|
| 1 | 0x93fdee74 Circuit | 0x790FD4 | 0 | yes |
| 2 | 0xdcb9f861 Sprint | 0x790FDC | 1 | yes |
| 3 | 0x35102edf Drag | 0x790FE4 | 2 | yes |
| 4 | 0xd7162cfa Drift | 0x790FEC | 3 | yes |
| 5 | 0x30da61e1 Street X | 0x791004 | 10 | yes |
| 6 | 0xc786c4e7 Underground Racing League | 0x79100C | 4 | yes |
| 7 | 0x613d3106 Outrun | 0x790FF4 | 8 | yes (context 8 / 0x200 / 4 only) |
| — | 0x6f2421b7 Free Run | 0x790FFC | 9 | **no** (offline only) |

The mode values come from the item activate functions 0x4B2BE0 … 0x4B2D20, each of which calls FUN_004b2c00(n). **(guess)** The exact pairing of each function address to each value was not tabulated. The vtable-to-text pairing in the table is exact.

### UI_QRTrackSelect.fng

| Item | Address |
|---|---|
| Builder | 0x4EF9B0 |
| Constructor | 0x4E2F30 |
| Vtable | 0x7974F0 |
| Handler | 0x4EFA10 |
| Track list | FUN_004cded0 |

- Title: 0x766a0edf "Track Select" in LAN. Online uses 0x3bc6066d.
- Help text: 0x27329d35.
- The chosen track goes to `0x8500b8`.

### UI_QRModeOptions.fng

| Item | Address |
|---|---|
| Builder | 0x4FA550 |
| Constructor | 0x4EF3F0 |
| Vtable | 0x798014 |
| Handler | 0x4EF620 |

- Title: 0x245448db "Race Mode Options" in LAN. Online uses 0x5d2b0b43.
- Help text: 0xf4ea444b.
- The option list is FUN_004e2c70, which calls **FUN_004b41c0** for every LAN and online mode.

Options in the order FUN_004b41c0 adds them:

| # | Label | Values | Global | Shown when |
|---|---|---|---|---|
| 1 | 0x744a80a7 Password protected? | 0x61c No / 0xf770 Yes | 0x85009b (reset to 0 on entry) | always |
| 2 | 0x9953843b Enter password | 12-char text entry; shows "********", or 0xbb298a80 "REQUIRED" | 0x85009c | always listed. Enabled only when protected (FUN_004982c0) |
| 3 | 0x97cb518b Confirm password | 12-char text entry | 0x8500a9 | enabled only when protected and a password is set |
| 4 | 0x81caa8e6 Number of Points to Win | 10, 20, 30, 40, 50, 100, Unlimited | 0x850090 (index) | Outrun only |
| 5 | 0xfe407a18 Time Limit | None, 2, 5, 8, 10 Minutes | 0x850094 (index) | Outrun only |
| 6 | 0xa88ffeb4 Track Direction | Forward / Reverse (0x4B40E0 / 0x4B4150) | 0x8500bc | not Outrun, track +0x86 == 1, and `FUN_005156f0(0x1f8, track+0x8a, 1, 0)` passes (reverse unlocked). Otherwise forced to 0 |
| 7 | 0x48494e83 Laps | min..10 (0x4B3E40 / 0x4B3F10). min = 2 for Circuit / URL, 4 for Drift / Street X (mode 5 also in the code) | 0x8500ba | not Sprint, Drag, Outrun, or point-to-point Drift (track +0x80 == 1). Otherwise laps = 1 |
| — | Ranked / Unranked | — | 0x850098 | **never in LAN**. Forced to 0 when the context is 0x200 or the mode is Outrun |
| 8 | 0xa40967b0 Maximum Players | 2..4. URL allows 2..6. Drag on track 0x1132 allows 2..3. Min +1 when ranked (online) | 0x850080 / 0x850081 | always |
| 9 | 0x7712e575 Collision Detection | On / Off | 0x85009a | always |
| 10 | 0x96a1e80f Car Performance Restrictions | 0xfabfc6dc Evenly Matched / 0xfac4999e None | 0x85008c | always |
| 11 | 0xb82a976e Race Breaker | On / Off | 0x850099 | not Drift or Street X (forced to 0) |
| — | 0x95684a90 Private Room | — | 0x8500c8 | **not in LAN** (online only) |

**Accept (msg 0x0c407210 on a non-text row)**
1. If protected, the password length must be 3..12. Otherwise it shows 0xd29a1a18 "Invalid password length. Password must be between %lu and %lu characters."
2. The password and the confirmation must match. Otherwise it shows 0xff99d845 "Entered passwords do not match."
3. FUN_00498440 sends msg 0x59337816 directly in LAN, which plays the FORWARD animation.
4. When the animation finishes (msg 0xe1fde1d1), the flow advances to **PC_OL_GameRoom.fng** (0x498220). The game session is then created from the game room (0xab13cae4 "The game session could not be created." at 0x4E47C7).

---

## 7. Race mode values (`0x850088`)

| Value | Mode | Text hash | Allowed in LAN |
|---|---|---|---|
| 0 | Circuit | 0x93fdee74 | yes |
| 1 | Sprint | 0xdcb9f861 | yes |
| 2 | Drag | 0x35102edf | yes |
| 3 | Drift | 0xd7162cfa | yes |
| 4 | URL (Underground Racing League) | 0xc786c4e7 (menu), 0x75f903f4 "URL Race" (lobby list) | yes |
| 8 | Outrun | 0x613d3106 | yes |
| 9 | Free Run | 0x6f2421b7 | **no** |
| 10 | Street X | 0x30da61e1 | yes |

Value 5 appears in the laps-minimum check but has no menu item. **(guess)** It is a leftover Street X id.

---

## 8. UI_OLCarSelect.fng in LAN

| Item | Address |
|---|---|
| Builder | 0x4FC190 (shared with QR car select) |
| Constructor | 0x4F9D90 |
| Vtable | 0x79770C |
| Handler | 0x4FC1F0 |

- Help text: 0xa7fa00b6. Title in LAN: 0x00d5f627 "Car Select".
- `OL_CarMode_Group` ("Race Mode:") is hidden in LAN.

### Car filters

The filter table is `DAT_007f444c`. The chosen filter is stored per player in 0x83aac0.

| Bit | Text |
|---|---|
| 1 | 0xe798a2f7 Stock Cars |
| 2 | 0xe7ab43d3 My Cars |
| 3 | 0x6b68362c All Cars |
| 4 | 0xb4045365 Career |
| 8 | 0x26fecee7 Sponsor |
| 0x10 | 0x1a23d5e6 Ranked Cars |

- The LAN constructor turns filter 0x10 into 3.
- An empty filter falls back to 3 (FUN_004eeb90).
- L/R (msg 0x5073ef13 / 0xd9feec59) runs FUN_004eed10. In the LAN/online branch, R cycles All -> Career -> Ranked -> My Cars -> Stock -> All, and L goes the other way. Career and My Cars are skipped when empty; Ranked is not (checked 2026-11-10): offline it holds no car, so the empty-filter rule above turns it into All.
- Port: UI_OLCarSelect is in main.c (isCarSelFng, lanCarSelAsk / lanCarSelKeep, lanRaceOwnCar), TASKS #321.
- The Career filter hides the stat bars 0x39b2b1c6 / 0xe8ca2e25.

### Accept (msg 0x406415e3 / 0x0c407210)

1. FUN_00498000 shows 0xc4f4678e "Do you want to race with this vehicle?".
2. Yes sends msg 0x43a743ab.
   - If the transmission-prompt flag 0x83aa6a is clear: the car (`this+0x1a8`) is stored and the game goes to **UI_OLMAIN**.
   - Otherwise FUN_00497d50 asks 0x1b3a2485 Manual / 0x61cb0720 Auto (msg 0x5f5e3886 / 0x1a2826e1 set 0x83aa79). Then msg 0xba5b5fce stores the car and goes to UI_OLMAIN.

### Other messages

- **Customize (msg 0xc519bfc0).** Uses the generic branch. If the player is over the car limit, it shows 0x91921a8d "You're over the limit of new cars you can customize…". Otherwise it sets the filter to 3 and opens customise with FUN_00552cc0(2, …).
- **Back (msg 0x911ab364 / 0x93b596ed).** Goes to UI_OLMAIN.

The car is chosen from the main menu before entering the lobby. The `invp` join error (section 5) is the server rejecting that car.

---

## 9. Game room to race (pre-race path)

The game room sub-screens are covered in the separate pass. These are the parts the flow needs.

### Start (FUN_004bb6e0)

The start is triggered from the game room. **(guess)** Only the host can trigger it. That matches the host and client sub-screens being separate (0x49FAA0 / 0x49FD20), but this pass did not prove it.

The function first calls `n = FUN_00582980()`. **(guess)** n is the number of players who are not ready.

- **`n == 0` and the countdown flag `this+0x1178` is not 1:** `FUN_0058aa20(cb 0x49F890)` starts the start countdown.
  - On success it sets net state 0x12.
  - On failure it shows 0x313f8cc5 "Could not start the game start countdown for some reason." (0x4BB73F)
- **Otherwise** (`n > 0`, or `this+0x1178 == 1`, meaning the countdown has run out):
  1. FUN_0058d0d0 runs. **(guess)** It removes the unready players.
  2. `FUN_0058a7e0(cb, this+0x1178)` starts the session.
  3. On success:
     - Online only: it posts "is playing %s" presence (0x679fd4e9 + "Underground 2"). LAN skips this, since the context is 0x200.
     - Sets net state 0x12.
     - Shows 0x9c6e60a4 "Starting Game Session...".
     - Stamps the time at `this+0x119c`.
  4. If the result is 0: hides widget 0xa6f52ee0 and shows 0xd616e55a "After removing unready players, there are insufficient players to start the game!"
  5. If the result is negative: shows 0xd99cfe5e "The game failed to start. Please try to create or join another game." (also at 0x4BB8F3)

**Rule:** the host can press Start while some players are not ready. The countdown then runs, and when it ends the unready players are dropped. The race starts only if enough players remain, meaning at least the min-players value at 0x850081. **(guess)** The link from "enough players" to 0x850081 is not shown in this code.

Other game-room strings, with where they are used:

| Location | String |
|---|---|
| 0x4E59D1, 0x4E970F | 0x9b8c9d7a "Race Countdown: %d" |
| 0x4D292F | 0xea6ea8da "Aborted". The countdown is cancelled; this sits inside an undefined function |
| 0x4D2985 | 0x7d162708 "The host has removed you from the game." |
| 0x4E4F56 | 0x7d1d448d "Kicking %s from the game." |
| 0x4E5A78 | 0xb62e143c "This game session is no longer available." (client) |
| 0x4E4D7D | 0xb419c854 "Retrieving car information ..." |

### UI_OLPreRaceStart.fng

| Item | Address |
|---|---|
| Builder | 0x4D3430 |
| Constructor | 0x4BC300 |
| Vtable | 0x794D94 |
| Handler | 0x4BC520 |

**Shown by FUN_005f94e0**, the network race state machine. When its state goes from 3 to 4, it pushes `UI_OLPreRaceStart.fng` (FUN_00555e80) and starts loading. For each of the **6** player slots in state 0/4/5/6, it initialises that player's race entry (`+0x974/+0x978/+0x998`). Then FUN_005838a0 / FUN_0058c270 runs. **(guess)** That call signals "loaded".

**The constructor fills, for up to 6 connected players** (`0x89cf50[0..5]`):
- `Player_Data_%d`: name at slot +0x12
- `Car_Data_%d`: `CAR_NAME_%s`
- `Time_Data_%d`

Unused rows up to 6 are blanked, and `Ranking_Data_1..6` are blanked.

**Handler 0x4BC520, tick msg 0xc98356ba:**
- If the net state `0x89cf48 == 5`, the screen dismisses itself and the race runs.
- Otherwise it refreshes the status with FUN_004a0360.
- After 5 ticks without a connection (FUN_005fe370), FUN_004a0200 shows a dialog with OK = msg 0x34dc1bec:
  - On a client: 0x4e7212d8 "The race can not be started because the connection to the host has been lost."
  - On the host (FUN_005f0120, `DAT_0086676c != 0`): 0x1eb7bf0e "The race can not be started because you have lost your network connection, or the other players cannot connect to your session."

**msg 0x34dc1bec** calls FUN_00502ff0(0), then FUN_0050c8d0 and FUN_00603790. Together they return to the frontend screen FUN_00502ff0 picks: for context 8 or **0x200 that is `PC_OL_Lobby.fng`** (0x502FF0). The game room is not reopened.

**Pause during the load.** FUN_00606730 handles the pause button. When the player is online or on LAN (FUN_005f00f0) and the net state is 4 (loading), Start toggles between `UI_Pause.fng` and `UI_OLPreRaceStart.fng`. It does not pause the race (0x606730 case 7).

**In-race pause in LAN.** The pause menu has 0xf39723a1 "Quit to LAN Main Menu" (0x4C2B4C). Its confirm is 0xd63b6982 "Are you sure you want to quit to the LAN main menu?" (FUN_004c2886 opens it as an InGameDialog with msg 0x96dda9e6).

### After the race

FUN_004e9210 is a results / status text updater. It runs only while online or on LAN (FUN_005f00f0). It shows one of:
- 0x90599828 "Waiting %d seconds for results" (0x4E9334)
- 0x30cbdcb6 "Waiting for final server results"
- 0xfa14812c "Starting next race in %d seconds"

**(guess)** The last message means a LAN session can chain races back to back, with the next race starting automatically after a timer. That would match the game-room "restart" behaviour; the main session's game-room pass should confirm it.

When a player leaves the session, the frontend return screen is again PC_OL_Lobby.fng (FUN_00502ff0, context 0x200).

---

## 10. Limits summary

| Limit | Value | Source |
|---|---|---|
| Max players, normal modes | 2..4 | 0x4B3110 / 0x4B3190 |
| Max players, URL | 2..6 | same |
| Max players, drag on track 0x1132 | 2..3 | same |
| Player slots in pre-race / net arrays | 6 | 0x4BC300, 0x5F94E0 |
| Lobby list rows visible | 15 | FUN_004d1380 |
| Games per list update | 20 | FUN_004bb0f0 |
| Name length | 12 typed, stored in 16-byte slots | FUN_004a2ff0, 0x4A2E20 |
| Password length | 3..12 | ModeOptions accept |
| Laps | 2..10 (Circuit / URL), 4..10 (Drift / Street X circuit), else 1 | 0x4B3E40 |
| Ranked | always off in LAN | FUN_004b41c0 |
| Private room | not offered in LAN | FUN_004b41c0 |
| LAN server port | 9900 (0x26AC), host connects to 127.0.0.1 | 0x4A2E20 tick |
| Joining a started game | refused (`asta`) | FUN_0049eb10 |

## 11. Notes for the 3DS rebuild

These follow from the evidence above. The design choices are ours.

- The PC splits LAN into host and join only at ServerSelect. Everything after that is shared with online, with checks on the context value. A 3DS port can keep one code path with `context = 0x200`.
- There is no lobby chat on PC. Text input in the lobby is list search only.
- The settings struct at 0x850080 is what a joiner needs from the host. It covers mode, track, direction, laps, max and min players, collision, restriction, race breaker, and the Outrun points and time limit.
