# Savefile format RE (task #67)

## Scope reminder

Same rule as `frontend-menu-re.md`: the goal is to recover NFSU2's *real* save format
(`UserProfile`'s in-memory layout, as read/written by `cFrontendDatabase`) well enough to port it, not
invent a new save schema for the 3DS port.

## Symbol source

Same technique as the frontend RE: `Gamecube Files/Speed.elf`'s unstripped symbol table, decompiled via
the reusable `tools/ghidra_scripts/DecompileFunc.java` headless recipe (see
`frontend-menu-re.md`'s "Ghidra headless decompile recipe" section — same invocation, just swap the
`addrs[]` list).

## Top-level entry points (session 24)

`cFrontendDatabase` embeds a `UserProfile` at `+0x18` (confirmed via `GetUserProfileSaveSize`,
`LoadUserProfileFromBuffer`, `SaveUserProfileToBuffer`, all trivial one-line forwarders). All the real
work is in `UserProfile`'s own `LoadFromBuffer`/`SaveToBuffer`/`GetSaveBufferSize` methods
(`80131f28`/`80131db4`/`80132130`).

**This is a plain sequential binary serializer, not a chunk/TLV format** (unlike `.fng`/`.BIN` — no tags,
no lengths on disk, just a fixed field order matching the in-memory struct almost exactly). Every
`Save/LoadFromBuffer` call takes `(buffer_ptr, offset_in, remaining_len)` and returns the advanced
offset; leaf fields go through `load_bytes__FPvT0i`/`save_bytes__FPvT0i` (a raw memcpy with a
remaining-length check, presumably clamps/no-ops if the save is short — not yet confirmed).

**`GetSaveBufferSize__11UserProfile`**: total size = `GetSaveBufferSize(FEPlayerCarDB) +
GetSaveBufferSize(PlayerCareerState) + 0xef0` (3824 fixed bytes for `UserProfile`'s own misc fields).

## `UserProfile::LoadFromBuffer` field order (`80131f28`)

In this exact order (offsets are into the live `UserProfile` struct, i.e. `cFrontendDatabase + 0x18 +
offset`):

1. `+0x49c` — `FEPlayerCarDB` (variable size, own sub-format below)
2. — `PlayerCareerState` — **not embedded in `UserProfile`, it's a separate global** `ThePlayerCareer`
   (own sub-format below); `Default__17PlayerCareerStateb(&ThePlayerCareer, 1)` is called first to
   reset it before loading over top
3. `+0x3c4`, 0xd8 (216) bytes -- **the EA Trax song table** (decoded 2026-10-24; the earlier
   "manufacturer category" reading was wrong). 27 records of `{u32 song, u8 play, 3 bytes pad}`.
   `song` indexes the exe's 27-entry track table (PC VA 0x7ECD80, 16-byte records: title, artist,
   album, bank); `play` is 0 Off / 1 Menu / 2 Racing / 3 All. The record order is the playlist
   order the jukebox lists and Sequential play walks. `Default__11UserProfileib` (`80131b54`; PC
   0x515AA0, loop at 0x515B46) writes song i at position i, with play taken from the track's bank:
   "FE" -> 1, "IG" -> 2, "AL" -> 3 (songs 0-5 are FE, 6-26 IG). The play order (Sequential 0 /
   Random 1) is not here but in `AudioSettings` +0x30.
4. `+0x4c`, 0x3c (60) bytes — raw block; passed to `UpdateVolumes__8EAXSoundP13AudioSettings` later in
   the same function when `param_4` (a "final load" flag) is set, so **this is an `AudioSettings`
   struct** — volume/audio config. One sub-field resolved (session 24 part 3): `+0x74` (within this
   block's range, `0x4c..0x88`) is conditionally defaulted in `Default__11UserProfileib` via
   `GetDefaultPlatformAudioMode__8EAXSound()` — **this is the platform audio mode** (e.g. mono/stereo/
   surround setting), only set when a particular global flag (checked at `unaff_r13-0x7bc4`, an
   unresolved TOC-relative global) is nonzero.
5. `+0x38`, 0x14 (20) bytes — raw block, not yet decoded
6. `+0x88`, 0x80 (128) bytes — raw block, not yet decoded
7. `+0x28`, 0x10 (16) bytes — raw block, not yet decoded
8. `+0x108`, 0x234 (564) bytes — **resolved (session 24 part 3)**: `Default__11UserProfileib` calls
   `Default__17QuickRaceSettings(profile+0x108)` — **this is a `QuickRaceSettings` struct** (quick-race
   mode configuration: presumably last-selected car/track/opponent-count/difficulty for the non-career
   "Quick Race" menu option). Internal field layout not yet decoded, but the struct identity is now
   known — worth a full decode once the quick-race menu screen is being built.
9. `+0x33c`, 4 bytes — **resolved (session 24 part 3)**: `Default__11UserProfileib` calls
   `Default__14CareerSettings(profile+0x33c)` — **this is a `CareerSettings` struct**, apparently just 4
   bytes on disk (a single field — plausibly a difficulty/assist-options enum for career mode).
10. `+0x340`, 4 bytes — raw u32; defaulted to 0 in `Default__11UserProfileib`, immediately after
    `CareerSettings` — semantics still unknown, but confirmed a separate field, not part of
    `CareerSettings` itself.
11. `+0x8894`, 4 bytes — raw u32; defaulted to 0 in `Default__11UserProfileib`.
12. `+0x8890`, 4 bytes — raw u32; defaulted to 1 in `Default__11UserProfileib` (a boolean-like flag,
    default-true — plausible candidate: "sound enabled"/"tutorial seen"/similar on-by-default toggle,
    not confirmed).
12b. `+0x8898`, 4 bytes — **newly identified (session 24 part 3), not previously in this list**: raw
    u32, defaulted to 1 in `Default__11UserProfileib`, set immediately before `+0x8894`/`+0x8890` in the
    same default-init run. Falls in the gap between the EA Trax song table (`+0x3c4..0x49c`, far
    below) and `HighScoresDatabase` (`+0x889c`, immediately after) — likely another standalone flag in
    the same cluster as `+0x8894`/`+0x8890`/`+0x888c`, semantics not confirmed.
13. `+0x888c`, 4 bytes — raw u32; defaulted to 0 in `Default__11UserProfileib` (set *after*
    `Default__18HighScoresDatabase`, i.e. physically and semantically separate from that block despite
    sitting right before it in memory).
14. `+0x889c`, 0xa50 (2640) bytes — **resolved as identity (session 24 part 3)**: immediately preceded
    in `Default__11UserProfileib` by `Default__18HighScoresDatabase(profile+0x889c)` — **this is a
    `HighScoresDatabase` struct** (matches the class name exactly; ties directly to the
    `CommitHighScoresPreRace`/`CommitHighScoresPostRace`/`CommitHighScoresPauseQuit` methods seen on
    `UserProfile` itself). Internal record layout (presumably an array of per-track/per-class high-score
    entries) not yet decoded.
15. `+0x344`, 0x80 (128) bytes — raw block; confirmed zero-defaulted (`bMemSet(profile+0x344, 0, 0x80)`)
    in `Default__11UserProfileib` — semantics still unknown beyond "zero by default."
16. `+0x0`, 0x20 (32) bytes — **resolved (session 24 part 3), correcting the earlier guess**: this is
    **NOT a version/magic header** as previously speculated — it is the **player's profile name**, a
    fixed 32-byte C-string buffer. Confirmed via `SetProfileName__11UserProfilePCcb` (`80131a80`, which
    `bMemSet`s these 32 bytes to 0 then `bStrNCpy`s up to 32 bytes of the given name in) and
    `GetProfileName__11UserProfile`/`IsProfileNamed__11UserProfile`. If no name is given,
    `SetProfileName` falls back to one of two localized default strings (string-table hashes
    `0xcf4f80c2`/`0xd87133bd`, selected by a caller-supplied flag — not yet resolved against
    `English.bin`, but this is the exact same hash-driven string system already confirmed for menu
    widget text). **This is very likely the single highest-value field for a "continue/select profile"
    main-menu screen.**
17. `+0x20`, 4 bytes — **resolved as identity, was previously folded into item 16's "raw block" guess**:
    this is the **"is profile named" flag** read by `IsProfileNamed__11UserProfile` (`*(u32*)(profile+
    0x20)`), written by `SetProfileName` every time it runs (1 if a real name was supplied, 0 if a
    localized placeholder was used instead). This is the same field documented below as being force-set
    to `1` immediately after every successful `LoadFromBuffer` call — now explained: loading a save
    always counts as "the profile has a name" regardless of what the on-disk name bytes actually
    contain, since `SetProfileName` isn't re-invoked on load (only the raw name bytes are copied), so
    the flag is set directly instead.
18. if `param_4` (final-load flag) is set: applies block 4's `AudioSettings` to `EAXSound`, initializes
    EA Trax (in-game soundtrack), and re-syncs controller/joystick configuration against the two
    connected `JoystickPort`s — **this step is player-facing initialization side-effects, not itself
    save-format-relevant**, but confirms field 4 = audio settings and shows loading a save has real
    runtime consequences beyond just copying bytes.
18. if `param_4` (final-load flag) is set: applies block 4's `AudioSettings` to `EAXSound`, initializes
    EA Trax (in-game soundtrack), and re-syncs controller/joystick configuration against the two
    connected `JoystickPort`s — **this step is player-facing initialization side-effects, not itself
    save-format-relevant**, but confirms field 4 = audio settings and shows loading a save has real
    runtime consequences beyond just copying bytes.

`SaveToBuffer` mirrors this exactly in the same order (as expected for a symmetric serializer).

## `FEPlayerCarDB::LoadFromBuffer` (`80134a9c`) — the player's owned-car garage

Fixed-count array serialization, no counts stored on disk (counts are compile-time constants):

1. **20x `FEStockCar`**, stride `0x1c` (28 bytes) each, starting at offset `+0x0` — likely the "cars
   available in dealership at this career stage" reference list (not owned cars — `FEStockCar` vs.
   `FECareerCar` naming), or unlock-state flags per stock model.
2. `+0x540`, 4 bytes — raw u32 (a count or index — e.g. "number of cars owned", plausible but not
   confirmed)
3. **20x `FECarConfig`**, stride `0x434` (1076 bytes) each, starting at `+0x544` — the actual **owned
   car garage slots**: 20 possible garage cars, each with a full `FECarConfig` (visual customization —
   paint, vinyl, parts — given the class name and the earlier frontend-RE findings about
   `CreateChoosePaintScreen`/`CreateChooseVinylLayerScreen` etc. reading from this same subsystem).
4. **5x `FECareerCar`**, stride `0x7fc` (2044 bytes) each, starting at `+0x5954` — likely the 5
   "signature"/career-tier cars (starter cars per career stage, or the cars actively raced in the 5
   career story chapters — not confirmed, needs `FECareerCar`'s own field decode).
5. `+0x8140`, 4 bytes and `+0x8144`, 4 bytes — two trailing u32s, not yet decoded (garage-wide counters,
   e.g. "currently selected/active car index" would be a natural guess given position at the very end).

Total `FEPlayerCarDB` size: `20*0x1c + 4 + 20*0x434 + 5*0x7fc + 8` = `0x8148` (33,096) bytes, matching
`param_1 + 0x8148` never being exceeded above (`0x8144` is the last field, +4 bytes = `0x8148`).

## `PlayerCareerState::LoadFromBuffer` (`8012a888`) — career progression

1. **8x `RuntimeCareerStageInfo`**, at `+0x69c4`, **variable per-entry size** (via
   `GetSaveBufferSize__22RuntimeCareerStageInfo`, not yet decompiled) but a fixed `0x104` (260-byte)
   struct stride in memory — the 8 career "stages"/story chapters' individual progress state.
2. `+0x681c`, 4B; `+0x7f58`, 4B; `+0x7e18`, 4B; `+0x7e30`, 4B; `+0x7e34`, 4B; `+0x71e4`, 4B; `+0x71e8`,
   4B — seven scattered raw u32s, not yet decoded (likely career-wide counters: total races won,
   current stage index, reputation/cash totals, etc. — needs individual investigation, non-contiguous
   offsets suggest they're genuinely separate named fields in the C++ struct, not one block)
3. `+0x71ec`, 0xc00 (3072) bytes — a large raw block, not yet decoded (by far the largest single field
   here — plausible candidate: a per-race or per-track completion/high-score table, given the size is a
   suspiciously round `0xc00 = 3072 = 48 tracks * 64 bytes` or similar factorable size, **not
   confirmed**, worth checking against the real number of tracks/events in the game)
4. `+0x7e28`, 4B; `+0x7e2c`, 4B; `+0x7e38`, 4B; `+0x7e3c`, 4B — four more scattered u32s
5. `+0x7e40`, 0x10 (16) bytes — raw block
6. `+0x7e50`, 2 bytes — a u16
7. `+0x7e54`, 4 bytes — a u32
8. `+0x7e58`, 0x100 (256) bytes — raw block
9. **`SMSMessageQueue`** at `+0x5408` (own sub-format, not yet decompiled — this is the in-game
   "text message" / pager system NFSU2 uses to deliver story/event notifications)
10. `+0x7f5c`, 0x10 (16) bytes — raw block; immediately after loading, the code counts how many of
    these 16 bytes are `\0` and stores the count at `+0x7f6c` — **strongly suggests this is a 16-entry
    array of 1-byte "slot used" flags or similar** (the loop is literally `for each of 16 bytes: if
    byte==0, increment a counter` — this is unusual enough to be a genuine structural clue, not just an
    arbitrary blob)
11. **`CareerSpeech`** at `+0x7f74` (own sub-format, not yet decompiled — likely which in-race voiceover
    lines have already been heard, to avoid repeats)
12. a final 4-byte field: on save, if `+0x7e24` (a runtime pointer, **not itself saved** — see below) is
    non-null, writes `*(u32*)(pointer+8)` (some ID/handle field read out of the live object); on load,
    reads that same 4-byte handle back and calls `FindCareerEventDataByHandle` to re-resolve the live
    pointer at `+0x7e24` — **this confirms `+0x7e24` is a "currently active career event" runtime
    pointer/cache that is deliberately not saved as a raw pointer, only as a stable lookup handle**, a
    real and correct save-format pattern (not a bug/gap) worth remembering when porting: any live
    pointer field in a struct being (de)serialized should be treated the same way, not naively
    memcpy'd.

## Sub-struct field layouts (session 24 part 2 — full depth)

### `FEStockCar` (28B / `0x1c`) — `80134968`/`801349a4`

```
+0x0, 0x18 (24B)  FECarRecord (base class, own LoadFromBuffer — not yet decompiled, treat as opaque
                  24-byte block for now; likely "which car model" id + basic record header)
+0x18, 4B         raw u32
```
`GetSaveBufferSize = GetSaveBufferSize(FECarRecord) + 4`, confirmed `FECarRecord` = 0x18 (24B) since
total is 0x1c.

### `FECarConfig` (1076B / `0x434`) — `8013476c`/`801347b8`

```
+0x0,  0x18 (24B)   FECarRecord (base, same as above)
+0x18, 0x418 (1048B) PresetCarSlot — the visual customisation payload. **Decoded**: a flat
                     `u32[0x106]` indexed directly by `CarSlotId`, each entry the bStringHash of an
                     installed *part name*. See "PresetCarSlot" below and
                     `docs/formats/car-parts-db.md` for what those names resolve to.
+0x430, 4B          raw u32
```

### `FECareerCar` (2044B / `0x7fc`) — `8013483c`/`8013489c`

```
+0x0,   0x434 (1076B) FECarConfig (embeds the above in full)
+0x7f4, 4B            raw u32
+0x7f8, 4B            raw u32
+0x434, 0x3c0 (960B)  OwnedPartInventory (own LoadFromBuffer, not yet decompiled — "which parts this
                      car owns", size 0x3c0 confirmed via GetSaveBufferSize arithmetic: 0x7fc - 0x434 -
                      8 = 0x3c0)
```
Note the two raw u32s at `+0x7f4`/`+0x7f8` are read *before* `OwnedPartInventory`, which is embedded
inside the `FECarConfig` region span (`+0x434`) — i.e. `OwnedPartInventory` physically overlaps/extends
past where `FECarConfig` ends, so the real field order on disk is: FECarConfig bytes, then the two u32s,
then OwnedPartInventory bytes (matches the code's read order, not a memory-overlap bug).

### `RuntimeCareerStageInfo` (260B / `0x104`) — `8012a56c`/`8012a698`

```
+0x0,   0x8c (140B)  raw block, not yet decoded
+0x8c,  0x10 (16B)   raw block
+0x9c,  0x28 (40B)   raw block
+0xc4,  0x18 (24B)   raw block
+0xdc,  4B           raw u32
+0xe0,  4B           raw u32
+0xe4,  4B           raw u32
+0xe8,  5x 4B (20B)  NOT raw — 5 sponsor-name hashes are read from disk (into a 20-byte scratch buffer)
                     and each is resolved via `FindSponsorDataByName__FUi` to a live pointer, which is
                     what's actually stored at +0xe8..+0xfc — same "handle not pointer" save pattern as
                     the career-event field documented below. On-disk bytes are 5 name hashes; in-memory
                     is 5 resolved sponsor-data pointers.
+0xfc,  4B           raw u32
+0x100, 4B           NOT raw — a stage number is read from disk and resolved via
                     `FindStageDataByStage__Fi` to a live pointer stored here (same handle pattern).
```
On-disk size and in-memory size are the same here (0x104) since the hash/pointer substitution is
same-width (4B in, 4B resolved-pointer out).

### `SMSMessageQueue` (5132B on disk / `0x140c`) — `8012cc30`/`8012ce1c`

The in-game "text message"/pager system. Loader allocates a 256-entry scratch array (0x1400 =
256 * 0x14), reads all 256 slots off disk, then **compacts** (drops empty slots, keeps order) into the
real struct, and recomputes two summary fields — this is not a plain memcpy like the other structs.

Per-slot layout (0x14 = 20 bytes each, x256 slots read from disk):
```
+0x0, 4B   message id — NOT raw: a 4-byte name-hash is read and resolved via
           `GetSMSMessageDataByNameHash__Fi` to a live pointer/id (handle pattern again).
           0 marks an empty slot (what the compaction pass drops).
+0x4, 1B   read      — 0 unread, 1 read
+0x5, 1B   popped    — the auto-popup for this message has been shown
+0x6, 1B   deleted
+0x7, 1B   sortable  — set on every read message when the mailbox sorts
+0x8, 4B   raw u32 (plausibly a timestamp)
+0xc, 4B   raw u32
+0x10, 4B  raw u32
```

**The four flag bytes, decoded 2026-10-22** — each from the one function that writes it, all in
the GameCube build:

| byte | writer | what it does |
|---|---|---|
| `+0x4` | `NotifyReadMessage__15SMSMessageQueueP10SMSMessage` `8012c588` | stores 1 unless it is already 1, and decrements the cached unread count at `+0x1404` |
| `+0x5` | `MarkAllAutoPopupsSeen__15SMSMessageQueue` `8012bcc4` | stores 1 for every message whose **data record**'s `+0x4` is 1, i.e. the auto-popup ones. `AnyUnseenAutoPopups` `8012bc38` is the matching test: data`+0x4`==1 and slot`+0x5`==0 |
| `+0x6` | `NotifyDeleteMessage__15SMSMessageQueueP10SMSMessage` `8012c610` | stores 1, sets `+0x4` to 1 as well, and decrements `+0x1404` when the message was neither read nor already deleted |
| `+0x7` | `MarkAllReadMessagesSortable__15SMSMessageQueue` `8012c3f4` | stores 1 on every slot whose `+0x4` is 1 |

**Unread** is therefore `+0x4 == 0 && +0x6 != 1`, which is exactly what
`GetNumMessagesUnread__15SMSMessageQueueb` (`8012c36c`) counts when its `cached` argument is false:

```c
int SMSMessageQueue::GetNumMessagesUnread(bool cached) {
    if (cached) return *(int*)(this + 0x1404);
    int n = 0;
    for (int i = 0; i < *(int*)(this + 0x1400); i++) {
        SMSMessage* m = GetQueuedMessageByIndex(i);
        if (m->read != 0) continue;
        if (m->deleted == 1) continue;
        n++;
    }
    return n;
}
```

So the two trailing summary fields are `+0x1400` = queued count and `+0x1404` = unread count.
Cross-checked against the user's own save: recounting the 256 slots gives **55 queued, 1 unread**,
and the file's own two summary fields read 55 and 1. `3DS Files/source/profile_save.c` recounts
rather than reading them, because the loader does.
After the 256 slots (0x1400 bytes), three more raw 4-byte fields are read directly off disk (total disk
size 0x1400 + 0xc = 0x140c, matching `GetSaveBufferSize`). Post-load, the code **overwrites** two
summary fields — count-of-non-empty-slots (via a compaction pass) and unread-count (via
`GetNumMessagesUnread__15SMSMessageQueueb`) — into the two of those three trailing fields, so those two
specific trailing values are effectively **recomputed, not trusted from disk** (defensive
re-derivation, same spirit as the career-event handle pattern: never trust a raw scalar that can be
recomputed from the entries themselves).

### `CareerSpeech` (472B / `0x1d8`) — `801893ac`/`801893f0`

Single raw `load_bytes` of the full 0x1d8 block — no field-level parsing in `LoadFromBuffer` itself.
Immediately after the raw copy, 4 individual 4-byte fields are force-zeroed:
`+0x54`, `+0x150`, `+0x158`, `+0x160`. These are presumably **runtime pointers/handles that live inside
the same byte range as the saved data** and must never be trusted from a raw copy — same defensive
category as the career-event/sponsor/stage handle patterns above, just resolved to "always null on
load" instead of "re-resolved via lookup". The rest of the 472 bytes (voice-line "already heard" state,
presumably a bitfield or per-line-id array) is still an undecoded blob — lower priority per the
user's/this doc's own prioritization (narrative/flavor system).

**Decoded 2026-11-04 (#268 I step 3).** The four zeroed fields are the neighbourhood the car was
last in and the three clocks' last readings, not pointers. The PC's block is the same fields in
the same order with the flags as bytes, which is where its 0x48 bytes went. PC offsets (payload
0xC28F + offset), GC offsets in brackets:

```
+0x00  u8      only Default writes it [GC +0x00, nothing reads it]
+0x01  u8      DDAY_EVENT_A done: Rachel's own Outrun lines are past [+0x04]
+0x04  s32     stage 0's step, 0..3 [+0x08]
+0x08  u8      event 5's line said (a stage 1 race won) [+0x0C]
+0x09  u8      FE event 4's first line said (the car lot) [+0x10]
+0x0A  u8      FE event 0x11's first line said (the crib) [+0x14]
+0x0B  u8      the sponsor list is up [+0x18]
+0x0C  u8      the first sponsor line said [+0x1C]
+0x0D  u8[11]  the DVD star lines said, by star count [+0x20, int[11]]
+0x18  u8      a DVD unlock line is queued [+0x4C]
+0x19  u8      the dearest locked cover's stars [+0x50]
+0x1A  u8      the Outrun voice, 0..4 [+0x51]
+0x1C  u32     the neighbourhood hash, zeroed on load [+0x54]
+0x20  s32     the career timer at the last neighbourhood line [+0x58]
+0x24  u8[3]   the stage 0 engage lines said (0xCA207583, 0xCA207584, 0x1BCB7AB0) [+0x5C, int[3]]
+0x27  u8      an engage line is speaking [+0x68]
+0x28  u8      FE event 2 raised (PartSelectionScreen) [+0x6C]
+0x2C  u32[30] the showcase unlocks announced (showcase +0x28) [+0x70]
+0xA4  u8      how many [+0xE8]
+0xA5  u8      the stage just started [+0xE9]
+0xA6  u8      the showcase mode's line said [+0xEC]
+0xA8  {s32 time, event, arg}[8]  the timed events, 1/4000 s [+0xF0]
+0x108 s32[6]  WorldTimer last / run, the career clock last / the career timer, RealTimer
               last / run; the three "last"s zeroed on load [+0x150]
+0x120 s32[28] each FE event's RealTimer run when last raised [+0x168]
```

SPEED2.EXE: SaveToBuffer 0x514530 (PlayerCareerState's, the block's 100 dwords), LoadFromBuffer
0x529230 (zeroes +0x1C, +0x108, +0x110, +0x118). The PC's methods on it: Default 0x531800,
HandleTick 0x551070, the timer tick 0x54BD30, HandleSpeechEvent 0x53E0B0, HandleFESpeechEvent
0x5243D0, the engage request 0x5251A0 and decline 0x511280, HandleSponsorSelect 0x525510 and
Done 0x5112A0, HandleShowcaseUnlock 0x5253F0, ShowcaseMode 0x525850, the Outrun start 0x511000 and
end 0x5110C0, the career event end 0x5242C0. The global CareerSpeech is 0x8635BC.

### `FECarRecord` (20B / `0x14`) — `801346a4`/`80134714` — base class of `FEStockCar`/`FECarConfig`

```
+0x0,  4B  raw u32
+0x4,  4B  raw u32
+0x8,  4B  raw u32
+0xc,  4B  raw u32
+0x10, 4B  raw u32
```
Five raw u32s, semantics not decoded (plausible guesses only: car model id, color/paint id, a
name/owner index, purchase price, purchase date — not confirmed). Note `FEStockCar`/`FECarConfig` both
place their next field at `+0x18`, i.e. **4 bytes of padding after this 0x14-byte base class** before
the derived class's own fields begin — not a bug, just struct alignment/padding, confirmed consistent
across both derived types.

### `OwnedPartInventory` (960B / `0x3c0`) — `80135bb8`/`80135c78` — embedded in `FECareerCar`

Read in this exact disk order (destination offsets are **not** monotonic — reads jump around within the
struct, unlike every other struct decoded so far — reported as observed, not reordered):
```
+0x0,   0x15c (348B) raw block
+0x15c, 0x10  (16B)  raw block
+0x16c, 0x100 (256B) raw block
+0x26c, 0x10  (16B)  raw block
+0x27c, 0x10  (16B)  raw block
+0x28c, 8B           raw block
+0x3bc, 4B           raw block  (jumps to near the very end of the struct)
+0x294, 8B           raw block  (jumps back to the middle)
+0x2ac, 0x110 (272B) raw block
+0x29c, 0x10  (16B)  raw block  (physically precedes +0x2ac, read after it)
```
Total 960B, matches `GetSaveBufferSize`. The non-monotonic destination pattern is a genuine structural
clue (not a transcription error) — plausibly reflects a class with multiple member sub-objects each
contributing their own `LoadFromBuffer` inline (compiler-inlined), where the sub-objects' declaration
order in the class doesn't match their memory offsets, or an array-of-structs being loaded
column-major. Not confirmed. Semantics of all fields unknown — no field-level decode attempted.

## What's genuinely still unknown

All 6 originally-listed sub-structs, plus `FECarRecord` and `OwnedPartInventory` underneath them, are
now structurally fully decoded (see above) — **every reachable struct's byte-level layout is now
known**, closing out the "structure" phase of task #67 entirely. What's still undecoded is purely
**semantics** (what a given raw block of bytes means), not structure:
- `FECarRecord`'s 5 raw u32s.
- `FECarConfig`'s `+0x18` 1048-byte raw customization block.
- `OwnedPartInventory`'s ten raw sub-blocks.
- The individual raw sub-blocks inside `RuntimeCareerStageInfo` (`+0`, `+0x8c`, `+0x9c`, `+0xc4`).
- ~~The per-slot flag bytes~~ and timestamp u32s inside `SMSMessageQueue` entries (semantic meaning, not
  structure — structure is fully known). **The four flag bytes were decoded 2026-10-22** — see the
  `SMSMessageQueue` section above. The three u32s are still raw; they are all zero in the user's save,
  so the file offers nothing to decode them against.
- The bulk of `CareerSpeech`'s 472-byte blob beyond the 4 zeroed handle fields.
- `UserProfile`'s remaining raw blocks: `+0x28` (16B), `+0x38` (20B), `+0x88` (128B), `+0x340` (4B),
  `+0x344` (128B, confirmed zero-defaulted), `+0x8894` (4B, confirmed zero-defaulted). **Session 24 part
  3 resolved several others** — see the field-order table above: `+0x0`/`+0x20` = profile name string +
  "is named" flag, `+0x3c4` = the EA Trax song table (decoded 2026-10-24), `+0x108` =
  `QuickRaceSettings` (identity known, internals not), `+0x33c` = `CareerSettings` (identity known),
  `+0x8890`/`+0x888c` = confirmed separate default-valued flags, `+0x889c` = `HighScoresDatabase`
  (identity known, internals not).
- `PlayerCareerState`'s ~15 scattered u32s and the `0xc00`-byte block at `+0x71ec` — unchanged from
  part 1, still not decoded.
- The 2 trailing `FEPlayerCarDB` u32s at `+0x8140`/`+0x8144` — unchanged from part 1.
- `QuickRaceSettings` and `HighScoresDatabase`'s own internal field layouts (struct identity now known,
  contents not yet decoded — same category as the still-unknown sub-struct internals above).

This remaining work is now entirely "raw block semantics" (what do these bytes *mean*) rather than
"what is the structure" — every struct reachable from `UserProfile::LoadFromBuffer`, all the way down,
now has a fully known byte-level layout. Full-depth structural RE for task #67 is complete; what's left
is optional semantic labeling of individual fields, which is not required to round-trip a save file
byte-for-byte and can be done incrementally as specific fields become relevant (e.g. when building a
profile-select screen that needs to display the player's name).

## Recommended next steps, in priority order for "get to a working main menu with real save state"

The user chose to RE the real format before building menu code (rejecting a save-stub shortcut), but
not every field above is equally relevant to that goal. Suggested prioritization for the *next*
session, cheapest-and-most-relevant first:

1. Decode `UserProfile`'s own raw blocks (`+0`, `+0x28`, `+0x38`, `+0x88`, `+0x108`, `+0x33c`/`+0x340`,
   `+0x344`, `+0x8890`-range, `+0x889c`) — these are the fields a main-menu profile-select/"continue"
   screen would plausibly read directly (player name, unlock flags, a version/magic header at `+0`).
   Small, self-contained, high payoff for the stated goal.
2. `FEStockCar`/`FECarConfig`/`FECareerCar` (garage) and `RuntimeCareerStageInfo`/career-progress u32s
   are needed for "which car does the player start with" and "what's the current career stage", both
   plausibly relevant to a first "press A → into game" milestone if that milestone starts a real
   career save rather than a hardcoded car — worth confirming with the user whether the first milestone
   needs this or can hardcode a car/track once a save format exists structurally.
3. `SMSMessageQueue`/`CareerSpeech` are lower priority — narrative/flavor systems, not blocking a first
   playable menu→game transition.

Do not attempt to guess field semantics from offsets alone without decompiling each struct's own
methods — this doc's "not yet decoded"/"plausible guess" callouts are explicitly flagged as unconfirmed
and should not be treated as ground truth by a future session.

---

# Part 2 — the real **PC** on-disk save (`SPEED2.EXE`), session 2026-09-13

Everything above was recovered from `Gamecube Files/Speed.elf` and describes the *in-memory*
`UserProfile` serializer. This part covers the thing the port actually has to read: the real
**PC** save files shipped in `PC Files/SAVE/NFS Underground 2/`, decompiled out of
`PC Files/SPEED2.EXE` (imagebase `0x400000`) and verified byte-for-byte against the user's own
five ground-truth files.

The GameCube build is still the *naming* source (its symbol table is unstripped); the PC build is
the *layout* source. Where the two disagree, the PC build wins for this port — field widths in
particular differ (several GC `u32` flags are `u8` on PC).

## Ground truth used

```
PC Files/SAVE/NFS Underground 2/Player/Player                     54,966 B   profile
PC Files/SAVE/NFS Underground 2/Player DVD 1/Player DVD 1      1,048,612 B   collectible
PC Files/SAVE/NFS Underground 2/Player DVD 3/Player DVD 3      1,048,612 B   collectible
PC Files/SAVE/NFS Underground 2/Player Magazine 1/...          1,048,612 B   collectible
PC Files/SAVE/NFS Underground 2/Player Magazine 2/...          1,048,612 B   collectible
```

**Directory convention (confirmed, not inferred):** every save is a *directory* whose single
contained file has the same name as the directory. Collectibles are siblings of the profile named
`<Profile> Magazine <N>` / `<Profile> DVD <N>`. This is the PC build emulating console memory-card
semantics on disk — each "file" on a memcard becomes a directory here.

## Container: `MC02`

```
+0x00  u32 magic     = 0x4D433032  ('MC02')
+0x04  u32 totalSize = the whole file's length, header included
+0x08  u32 sizeA     = size of block A (always 8)
+0x0C  u32 sizeB     = size of block B
+0x10  u32 sumA      = checksum(block A)
+0x14  u32 sumB      = checksum(block B)
+0x18  u32 sumHdr    = checksum(header bytes [0x00..0x18))
+0x1C  block A       = { u32 0x10D, u32 sizeB }        (8 bytes)
+0x24  block B       = the payload
```

Writer: `FUN_004e8d30` -> `FUN_004d9b80`/`FUN_004f3570` -> `FUN_004d9c60` -> `FUN_006fbf13` ->
`FUN_006fce82` -> `FUN_006fc226` (the header writer).
Reader: `FUN_005662c0` -> `FUN_004f4a20` -> `FUN_006fbff1` -> `FUN_006fdb8d` -> `FUN_006fd2f6`.

`FUN_006fd2f6` (validation) rejects the file unless **all** of:
`hdr[1] == __filelength(fd)`, `hdr[0] == 0x4d433032`, `crc(hdr, 0x18) == hdr[6]`,
`crc(A, sizeA) == hdr[4]`, `crc(B, sizeB) == hdr[5]`.
Error codes it returns: `7` = I/O error, `8` = corrupt, `9` = open failed.

### Checksum

`FUN_006fcd1c` is a thunk to `FUN_006fe173` @ VA `0x6fe173`:

```c
uint32_t mc_crc(const uint8_t *p, int len)
{
    if (len < 4) return 0;
    uint32_t v = ~((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
    for (int i = 4; i < len; i++)
        v = ((v << 8) | p[i]) ^ crc_table[v >> 24];
    return ~v;
}
```

`crc_table` is `TABLE_008145e0` (file offset `0x4145e0`, `.data`) — the standard **MSB-first CRC-32
table for polynomial `0x04C11DB7`**, so it can be generated at runtime instead of embedded. Note the
unusual *first-four-bytes-preload* formulation: this is not stock CRC-32, and a stock implementation
will not reproduce these checksums. **Verified: all three checksums match on all five ground-truth
files.**

## Collectibles: magazines and DVDs

Same `MC02` container. Block B is exactly **1,048,576 bytes = 512 x 512 x 4** — a raw
**BGRA8888** image (little-endian `0xAARRGGBB`), i.e. byte order `B,G,R,A`. Confirmed by channel
statistics: byte 3 is the only bimodal channel (192 distinct values, 193,364 zeros and 49,660
`0xFF`s) so it is alpha, and the resulting render has the EA GAMES badge with **blue "EA" / green
"GAMES"** — rendering it as RGBA swaps R and B and turns the badge orange.

These are rendered cover screenshots the game captures and stores, one per collectible, not
compressed art. For the 3DS port this is a 1 MiB blob per collectible at 512x512 — a prime
candidate for the standing downscale-first rule (a 256x256 RGB565 re-encode is 128 KiB, 1/8th).

## Payload size

`FUN_0052a440` = `UserProfile::GetSaveBufferSize` = `FUN_005b9680() + 0xd254` = `0x43e + 0xd254`
= **`0xD692` = 54,930 bytes, a compile-time constant on PC**. `FUN_0052a450` thunks to
`FUN_0052a280` = `UserProfile::SaveToBuffer`.

54,930 + 0x24 header + 8 block A = 54,966 = the real file size of `Player/Player`. OK

## Block B — full field map

Independently anchored three ways: the literal string `"Player"` lands exactly at payload
`0xD201`; the 27 EA Trax song records at `0xC423` decode cleanly as `{song 0..26, play 0..3}`;
and the `0x64` (`'d'`) filler markers inside the car-config blocks land exactly on the predicted
`0x430`/`0x7F2`/`0x434` strides.

```
field                                     offset    size     PC serializer
--------------------------------------------------------------------------------
FEPlayerCarDB                             0x000000  0x993E   FUN_005166b0
  FEStockCar[48]                          0x000000  0x480     (0x18 each; mem stride 0x1C)
  u32 numStockCars                        0x000480  0x4       (= 31 in Player)
  FECarConfig[20]  (tuned-car garage)     0x000484  0x53C0    (0x430 each; mem stride 0x434)
  FECareerCar[5]                          0x005844  0x27BA    (0x7F2 each; mem stride 0x7F8)
  u32  (FEPlayerCarDB+0x812c)             0x007FFE  0x4       (= 5, career-car slots unlocked)
  FECarConfig[6]   (online career cars)   0x008002  0x1938    (0x434 each; mem stride 0x438)
  u32  (FEPlayerCarDB+0x9a80)             0x00993A  0x4       (= 6)
PlayerCareerState                         0x00993E  0x2AE5   FUN_00514530
  RuntimeCareerStageInfo[8]               0x00993E  0x808      (0x101 each; mem stride 0x104) FUN_00500fa0
  body                                    0x00A146  0xD30      (see below)
  SMSMessageQueue                         0x00AE76  0x1409     (256 x 0x14, then u32,u32,u8) FUN_005014f0
  tail                                    0x00C27F  0x1A4      (see below)
EATraxSong[27]   {u32 song, u8 play, pad} 0x00C423  0xD8
AudioSettings                             0x00C4FB  0x3C
blob (UserProfile+0x38)                   0x00C537  0x10
blob (UserProfile+0x84)                   0x00C547  0x50
blob (UserProfile+0x28)                   0x00C597  0x10
QuickRaceSettings                         0x00C5A7  0x17C
CareerSettings (u32)                      0x00C723  0x4       (= 1 in Player)
u32 (UserProfile+0x254)                   0x00C727  0x4
u8  (UserProfile+0xA0F9)                  0x00C72B  0x1
u8  (UserProfile+0xA0F8)                  0x00C72C  0x1       (= 1 in Player)
u32 (UserProfile+0xA0F4)                  0x00C72D  0x4
HighScoresDatabase                        0x00C731  0xA50
blob (UserProfile+0x258)                  0x00D181  0x80
ProfileName[32]                           0x00D201  0x20      ASCII, NUL-padded
ProfileFlags                              0x00D221  0x33      FUN_00515bf0
InputDevice[6]                            0x00D254  0x43E     FUN_005b9420  ** PC-ONLY **
--------------------------------------------------------------------------------
TOTAL                                               0xD692 = 54,930  matches GetSaveBufferSize
```

`ProfileFlags` (`FUN_00515bf0`, 0x33 bytes) is: `u32 0`, `u32 0`, `u8 +0x49`, `u8 +0x4a`,
`u8 +0x4b`, `5 x u32` from `+0x58`, `5 x u32` from `+0x80`. In `Player` only the two bytes at
block offsets 8 and 9 are non-zero (both `1`).

`InputDevice[6]` (`FUN_005b9420`, 6 x `0xB5` = `0x43E`) is **PC-only**: each record is a 16-byte
DirectInput device GUID + 9 `u32` + `u8` + a 0x80-byte button-mapping table. `GUID_SysMouse` is
literally present at payload `0xD254`. The backing global array is at `0x874f84`, stride `0x370`,
ending `0x876424`. This block is the whole reason the PC payload (54,930) is larger than the
GameCube one (48,018), and it is the block the 3DS port replaces with its own mapping.

### Sub-serializer sizes (each verified against raw disassembly)

| PC VA | struct | disk size |
|---|---|---|
| `FUN_005166b0` | `FEPlayerCarDB` | `0x993E` |
| `FUN_00514530` | `PlayerCareerState` | `0x2AE5` |
| `FUN_00500fa0` | `RuntimeCareerStageInfo` | `0x101` (mem stride `0x104`) |
| `FUN_00516620` | `FECareerCar` | `0x7F2` = `0x14` + `0x418` + `4` + `1` + `1` + `0x3C0` |
| `FUN_00503820` | `FECarRecord` | `0x14` (5 x u32) |
| `FUN_00503f00` | `OwnedPartInventory` | `0x3C0` (0xF0 x u32) |
| `FUN_005014f0` | `SMSMessageQueue` | `0x1409` |
| `FUN_00515bf0` | profile flags | `0x33` |
| `FUN_005b9420` | `InputDevice[6]` | `0x43E` |

> **Correction to Part 1.** Part 1 states `FECarRecord` is `0x18` on disk because `FEStockCar` is
> `0x1c`. That is wrong. `LoadFromBuffer__11FECarRecord` (`801346a4`) reads exactly **five** u32s
> = `0x14`, and `__11FECarRecord` (`80135140`) writes the vtable pointer to `param_1[5]`, i.e.
> `+0x14` — so the "4 bytes of padding after the base class" noted in Part 1 is the **vtable
> pointer**, which is never serialized. `FEStockCar` is therefore `0x14 + 4 = 0x18` on disk and
> `0x1c` in memory. The PC file confirms this directly: 48 stock-car records at a `0x18` stride
> sum to exactly `0x480`.

## Field semantics — decoded (session 2026-09-13)

Names come from the GameCube symbol table; PC offsets come from the PC serializer read order,
matched positionally against `LoadFromBuffer__17PlayerCareerState` (`8012a888`). The two orders
have the same field *count* in every group, so the positional mapping is unambiguous.

### `FECarRecord` (the 5 u32s, shared base of every car record)

From `Initialize__11FECarRecordUii` (`80135174`), `__11FECarRecord` (`80135140`),
`MatchesFilter__11FECarRecordi` (`80135198`) and `FillFromStockCar__11FECarConfig...` (`80135388`):

```
+0x00  u32    (constant, 0)
+0x04  u32    record name hash  -- "STOCK_<CAR>" / "CAREER_SLOT_<n>" / "ONLINE_CAREER_<MODE>"
+0x08  float  performance measure -- GetPerformanceMeasure__15RidePhysicsInfo(), 0 until computed
+0x0C  u32    in-use flag (set to 1 by FillWithData / DefaultCareerCars / FillFromStockCar)
+0x10  u32    filter bitmask -- 1 = stock, 2 = tuned, 4 = career, 16 = online career
       (+0x14 in memory = vtable pointer, NOT saved)
```

`FEStockCar` adds `u32 carType` (a 0-based index into the car-data table, stride `0x890`) at disk
`+0x14`. `GetCarType__10FEStockCar` (`8018b71c`) = `*(u32*)(this + 0x18)` in memory.

`FECarConfig`/`FECareerCar` instead carry a **car-type name hash** at disk `+0x1C`
(`GetCarType__11FECarConfig` (`80135524`) reads memory `+0x20` and resolves it through
`GetCarTypeInfoFromHash`, returning `carTypeInfo + 0x840`). Memory `+0x18` is a `PresetCarSlot`
(the customisation payload), so disk `+0x14` is `PresetCarSlot+0x00` and disk `+0x1C` is
`PresetCarSlot+0x08`.

### `PlayerCareerState` body (payload `0x00A146`, `0xD30` bytes)

| payload | PC field | width | GC equivalent | meaning | `Player` |
|---|---|---|---|---|---|
| `0x00A146` | `+0x7134` | u32 | `+0x681c` | **`CareerWallet` balance — the player's cash** | `388` |
| `0x00A14A` | `+0x8868` | u8 | `+0x7f58` | flag | `1` |
| `0x00A14B` | `+0x872c` | u32 | `+0x7e18` | **races entered** (`NotifyEnterEvent` 0x541160 adds one per event start; `FillStatistics` reads it) | `26` |
| `0x00A14F` | `+0x873e` | u8 | `+0x7e30` | flag | `0` |
| `0x00A150` | `+0x873f` | u8 | `+0x7e34` | flag | `1` |
| `0x00A151` | `+0x7af8` | u32 | `+0x71e4` | **current career stage index** (0..7) | `3` |
| `0x00A155` | `+0x7afc` | u32 | `+0x71e8` | **number of event state records in use** | `45` |
| `0x00A159` | `+0x7b00` | `0xC00` | `+0x71ec` | **`StateRecord[384]`**, 8 B each | 45 used |
| `0x00AD59` | `+0x873c` | u8 | `+0x7e28` | **career started** (PC byte `[0x86347C]`, GC `DAT_803bbecc`): set by `DoWhatNext` mode 6 (GC 0x80183E80) once the first career movie has played; while clear, `StartCareerHelper` (GC 0x800D1B30 / PC 0x495A30) plays `scene01` instead of entering free roam, and `UIStartCareer::Setup` offers Start New Career alone. Port: `Profile.careerStarted`, `B_CAREER_STARTED` (body B 0xC13) | `1` |
| `0x00AD5A` | `+0x873d` | u8 | `+0x7e2c` | **career has a car of its own** (GC `DAT_803bbed0`): clear, `UIMain` shows `DDAY_PLAYER_CAR` and `InitTopologyAndSceneryGroups` (0x80196cb8) enables `FREE_ROAM_STAGE0A` on stage 0 instead of `FREE_ROAM_STAGE0`; PC `MainFNGObject` tests byte `[0x86347D]` | `1` |
| `0x00AD5B` | `+0x8740` | u32 | `+0x7e38` | **current career car handle** | `0x319A8CA2` = `CAREER_SLOT_1` |
| `0x00AD5F` | `+0x8744` | u8 | `+0x7e3c` | flag: **the saved respawn is valid** (GC `DAT_803bbee0`). `StartCareerHelper`'s free-roam arm and `NotifyEnterCribFromPauseMenu` set it; `StartCareerFreeRoam` (GC 0x8011CBAC) uses the respawn below only when it is set, otherwise track 4000's grid slot | `0` |
| `0x00AD60` | `+0x8750` | `0x10` | `+0x7e40` | **saved respawn position** (`bVector3` + pad) | `(-1321.96, 694.23, 39.41)` |
| `0x00AD70` | `+0x8760` | u16 | `+0x7e50` | **saved respawn heading** (`0..0xFFFF` = full turn) | `0xD800` ~ 303.75 deg |
| `0x00AD72` | `+0x8764` | float | `+0x7e54` | **career clock**, seconds: `PlayerCareerState::UpdateTick` (PC 0x551320) adds the world's time step in career free roam while the player has the car, and `SMSMessageQueue::Tick` (PC 0x529750) queues each trigger 3 message it has passed. `Default` (PC 0x532830) sets -1.0. Port: `Profile.careerTime`, `B_CAREER_TIME` (body 0xC2C) | `3541.97f` (9738.3 by 2026-11-03) |
| `0x00AD76` | `+0x8768` | `0x100` | `+0x7e58` | **bank-pickup claimed flags[256]**, 1 byte each | 12 set |

`StateRecord` (`AddStateRecord`/`GetStateRecord`/`UpdateStateRecord__...25eUG2EventCompletionStates`,
`8012386c`/`801238c0`/`80123904`), 8 bytes, 384 slots — `Default__17PlayerCareerStateb` initialises
exactly `0x180` = 384 of them at stride 8:

```
+0x00  u32  career-event name hash   (e.g. DDAY_EVENT_A = 0xDD60E402)
+0x04  u8   state byte 0
+0x05  u8   state byte 1
+0x06  u8   completion state -- CommitWinEvent__17PlayerCareerStateUi sets this to 1 on a win
+0x07  u8   state byte 3
```

Cross-check: `Player` has exactly **45** non-empty records and the count field reads **45**.

The respawn position/heading pair is read back by
`GetRespawnParameters__17PlayerCareerStateP8bVector3PUs` (`8012765c`) and written by
`SaveSpawnPosition__17PlayerCareerState` (`801266dc`).

The 256 bank flags are written by `ClaimBankReward__17PlayerCareerStatePC17CareerBankTrigger`
(`80126250`) as `*(u8*)(this + 0x7e58 + trigger[3]) = 1` — one byte per `CareerBankTrigger`, indexed
by a byte in the trigger's own data record.

### `PlayerCareerState` tail (payload `0x00C27F`, `0x1A4` bytes)

```
0x00C27F  0x10   16 flag bytes, default 1 each (GC +0x7f5c; GC then counts the zeros into +0x7f6c)
0x00C28F  0x190  CareerSpeech, PlayerCareerState +0x887C copied raw (GC struct is 0x1d8; the layout
                 is in the CareerSpeech section)
0x00C41F  0x4    active career-event handle; 0 = none. Deliberately saved as a lookup handle, never
                 as the live pointer -- reloaded via FindCareerEventDataByHandle.
```

### `CareerWallet`

A single `int`. `GetBalance__12CareerWallet` (`8012a040`) returns `*this`;
`Deposit__12CareerWalleti` adds; `Spend__12CareerWalleti` subtracts and **clamps to 0**;
`Default__12CareerWallet` seeds it from `GetEasterEggPreBonusAmount__Fv()`.

Independently confirmed on the PC side by scanning `SPEED2.EXE` for `+0x7134` accesses:

```
00528a0b  SUB dword ptr [ESI + 0x7134],EDI   ... then MOV [ESI+0x7134],0   -> Spend, with the clamp
00532d19  ADD dword ptr [ESI + 0x7134],ECX                                 -> Deposit
00528a66  MOV EAX,dword ptr [ECX + 0x7134]                                 -> GetBalance
00551351  MOV dword ptr [ESI + 0x7134],0xf4240                             -> 1,000,000 easter egg
```

`bUnlimitedCasholaz` (GC `8046dca8`) is the matching debug global.

## What `PC Files/SAVE/NFS Underground 2/Player/Player` actually contains

All values below are read, not guessed:

```
profile name              "Player"
cash                      $388
career stage              3
career events recorded    45  (of 384 slots)
bank pickups claimed      12  (of 256)
stock cars known          31  (of 48 slots) -- carType 0..30, STOCK_PEUGOT .. STOCK_IMPREZAWRX
tuned-car garage          0 of 20 slots used
career car slots unlocked 5
  CAREER_SLOT_0           Corsa                 (perf 176.36)
  CAREER_SLOT_1           Tiburon               (perf 207.51)   <- current car
  CAREER_SLOT_2..4        empty
online career cars        6 (MIATA, FOCUS, 240SX, COROLLA, PEUGOT, FOCUS -- fixed defaults)
saved respawn             (-1321.96, 694.23, 39.41), heading 0xD800
CareerSettings            1
collectibles              Magazine 1, Magazine 2, DVD 1, DVD 3
```

## `PresetCarSlot` — the customisation payload (0x418)

Every `FECarConfig` embeds one of these at `+0x18`, so on disk it starts at record `+0x18`
and its first word lands at record `+0x1C` — which is exactly the `carTypeHash` field this
document already had. That is not a coincidence, it is the alignment: **`PresetCarSlot` is a
flat `u32` array indexed directly by `CarSlotId`, and slot 0 is the car type.** 0x418 / 4 =
0x106 entries, which is `CarSlotId`'s range with room to spare.

Each entry is the **bStringHash of an installed part's name**, not a value. A save stores no
colour, no geometry and no texture — only names. `docs/formats/car-parts-db.md` describes the
database in `GLOBALB.BUN` those names resolve against.

### Where `CarSlotId` comes from

Not guessed and not inferred from string order: `SPEED2.EXE` carries the enum as a literal
table of `{const char* name, u32 id}` pairs at file offset **`0x402e1c`** in `.rdata`, 8-byte
stride, 269 entries. Ids run `WIDE_BODY` = 7 through `WHEEL_MANUFACTURER` = 0xa9, with a
second adjacent table supplying 0–6 (`MISC`, `BASE`, `FRONT_BUMPER`, `REAR_BUMPER`,
`LEFT_SIDE_MIRROR`, `RIGHT_SIDE_MIRROR`, `BODY`).

**Do not reconstruct this enum from the string pool.** The linker pools and shares string
literals, so address order in the pool is *not* enum order; an earlier attempt to derive the
slot numbering that way came out five entries short (it silently dropped `ENGINE`,
`HEADLIGHT`, `BRAKELIGHT`, `EXHAUST` and `SPINNER`, which happen to be shared literals) and
produced an off-by-one that looked self-consistent. The pointer table is the authority.

### The paint and vinyl slots

| Id | Name | | Id | Name |
|---|---|---|---|---|
| `0x40` | `BASE_PAINT` | | `0x4A` | `PAINT_RIM` |
| `0x41`–`0x44` | `VINYL_LAYER0`–`3` | | `0x4B` | `PAINT_SPINNER` |
| `0x45` | `PAINT_ENGINE` | | `0x4C` | `PAINT_ROOF` |
| `0x46` | `PAINT_SPOILER` | | `0x4D` | `PAINT_MIRROR` |
| `0x47` | `PAINT_BRAKE` | | `0x4E`–`0x5D` | `VINYL_COLOUR0_0`–`3_3` |
| `0x48` | `PAINT_EXHAUST` | | `0x5E`–`0x61` | `KIT_CARBON`, `HOOD_CARBON`, `DOOR_CARBON`, `TRUNK_CARBON` |
| `0x49` | `PAINT_AUDIO` | | | |

### How the slot→array alignment was confirmed

Four independent anchors in the user's own save, each of which would break under an off-by-one:

1. `FRONT_BRAKE` and `REAR_BRAKE` hold the **same** hash — brakes are bought as a pair.
2. `FRONT_WHEEL` and `REAR_WHEEL` likewise.
3. Exactly **sixteen** `VINYL_L<n>_COLOR<m>`-named parts occupy exactly the sixteen
   `VINYL_COLOUR<n>_<m>` slots, in order.
4. The four `*_CARBON` slots hold four identical hashes, and `BASE_PAINT` holds a `GLOSS_*`
   part — a paint name in the paint slot.

### Decoded values from the reference save

```
CAREER_SLOT_0  CORSA     BASE_PAINT -> rgb(6,20,70)    + roof/spoiler/mirror/rim/spinner colours
CAREER_SLOT_1  TIBURON   BASE_PAINT -> rgb(30,55,90)   (0xedcd9ac6)
```

`profile_save.c` reads these into `ProfileCarRecord` (`basePaint`, `paintRoof`, `paintSpoiler`,
`paintMirror`, `paintRim`, `paintSpinner`, `vinylLayer[4]`), guarded by a stride check so a
record too short to contain the array is simply not read from.

## Still undecoded (PC)

Structure is complete; these are *semantics* only, and per this project's standing rule they stay
at their confirmed defaults rather than being guessed:

- `RuntimeCareerStageInfo`'s `0x101` body beyond the sponsor-hash/stage-number handle fields
  already documented in Part 1. The completed byte at `+0xFC` is decoded (2026-10-25, below).
- ~~The `0x418` `PresetCarSlot` customisation payload inside every `FECarConfig`.~~ **Decoded** —
  see the section below. What remains undecoded there is not the payload but the *vinyl art*: the
  four `VINYL_LAYER<n>` slots name parts whose shapes live nowhere in the retail tree as bitmaps
  (see `docs/formats/car-parts-db.md`, "Vinyls are not textures").
- `OwnedPartInventory`'s ten sub-blocks.
- `QuickRaceSettings` (`0x17C`) internals, and the last `0x50` bytes of `HighScoresDatabase`
  (`0xA50`); its 320 records are decoded (2026-10-25, below).
- The six single-byte flags in the `PlayerCareerState` body. (`+0x872c` is the races entered and
  `+0x8764` the career clock, both decoded since.)

## Stage completion and high scores (decoded 2026-10-25)

Both are read by `CribWorldMapScreen`; see `docs/formats/crib-world-map-re.md`. Neither is read by
`profile_save.c` yet.

**`RuntimeCareerStageInfo` completed byte.** At payload `0x993E + s * 0x101`, the byte at `+0xFC`
is 1 once stage `s` is finished (HasCompletedStage). The stage number follows it as an s32 at
`+0xFD`. The reference save "Player" has stages 0, 1 and 2 at 1 and stages 3..7 at 0.

**`HighScoresDatabase` records.** At payload `0xC731`, `0xA50` bytes. The first 320 entries are
8-byte records:

| Offset | Type | Field |
|---|---|---|
| +0 | s16 | track id |
| +2 | u8 | category (the lookups below ignore it) |
| +3 | u8 | kind (1 = reversed) |
| +4 | s32 | value: ticks (4000 per second) for a time, points for a score |

Two lookups read them:

- **Best time:** PC 0x502AA0 returns the smallest value over the records that match (track, kind).
  With no match, it falls back to the track's par time.
- **Best score:** 0x502C50 returns the largest value. When that is 0, it falls back to the par
  score at TrackInfo `+0xA4` (kind 0) or `+0xA8` (kind 1).

The reference save holds 37 records, e.g. track 4101, category 1, kind 0 = 516520. The remaining
`0x50` bytes are not decoded.
