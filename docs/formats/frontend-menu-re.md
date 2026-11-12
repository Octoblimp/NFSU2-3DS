# Frontend/menu system RE notes (task: RE the front-end menu flow/data)

## Scope reminder

Per project scope (see top of `HANDOFF.md`), the goal here is NOT to design a
new menu system for the 3DS port. It is to recover the *real* NFSU2 frontend
(screens, layout, navigation flow, and backing data) well enough to port it
faithfully. No menu code should be written for the 3DS side until this RE
work has produced something concrete to port.

## Symbol source

`Gamecube Files/Speed.elf` is an unstripped CodeWarrior debug build with a
full (mangled C++) symbol table -- this is the primary source for frontend RE,
much faster than working from the stripped/obfuscated PC `SPEED2.EXE`.
Symbols were pulled with WSL's generic (non-devkitPPC) `binutils`, which reads
PowerPC ELF symbol tables fine even without a PPC-specific toolchain
installed:

```
wsl.exe -e bash -lc 'cd "<repo>/Gamecube Files" && nm Speed.elf | grep -i ...'
```

`readelf` is also available the same way if section/relocation data is needed
later.

## Architecture confirmed from symbol survey (session 12)

The frontend is a full data-driven screen framework, not a handful of
hardcoded menu functions:

- **`GameFlowManager`** -- top-level orchestrator. Symbols include
  `LoadFrontend__15GameFlowManager`, `GameFlowLoadingFrontEndPart1/2/3__Fi`,
  `BeginGameFlowLoadingFrontEnd__Fv` / `EndGameFlowLoadingFrontEnd__Fv`,
  `CheckForDemoDiscTimeout__15GameFlowManager`,
  `ClearWaitingForCallback__15GameFlowManager`. This is the state machine
  driving which screen is active and how loading/transitions happen.
- **`cFrontendDatabase`** -- holds player/profile/career state the frontend
  reads and writes (`BuildCurrentRideForPlayer`, `CalcRaceTypeForHighScores`,
  `IsCurrentRaceEventInCareerMode`, `GetUserProfileSaveSize`,
  `LoadUserProfileFromBuffer`, `IsOnlineMode`/`IsLANMode`, etc.) -- this is
  the data model behind menus like career/garage/profile, not visuals.
- **`ScreenConstructorData`** -- every concrete screen has a
  `Create<ScreenName>Screen(ScreenConstructorData*)` factory function (~80+
  found so far, see below). This is almost certainly built from a
  table/registry keyed by screen ID or name string, likely populated at
  startup or read from a resource -- finding that registry/table is the next
  concrete RE step (see Open Questions).
- **`FEObject`** -- base UI object type; widgets subclass it. Concrete widget
  types found: `FEWidget`, `FEButtonWidget`, `FESliderWidget`, `FEInputWidget`,
  `FEDateWidget`, `FEKeyboard`. Each has message-handling functions like
  `NotifySoundMessage__<Type>UlP8FEObjectUlUl18FrontendSoundEvent` and
  `DetermineSpecialSound__<Type>Ul18FrontendSoundEvent`, implying a shared
  message-dispatch/event system across all widgets (worth diagramming once a
  couple of these are decompiled).
- **`MenuScreen`** -- appears to be the concrete base/leaf class most screens
  use (`NotifySoundMessage__10MenuScreenUlP8FEObjectUlUl18FrontendSoundEvent`).

## Screen inventory (partial, from `Create*Screen`/`Create*Menu` symbols)

This list is representative, not exhaustive -- there are 80+ `Create*`
factory functions. Grouped by apparent area:

- **Boot/legal/intro**: `CreateEALogoBumperScreen`, `CreateESRBScreen`,
  `CreateLegalScreen`, `CreateLanguageSelectScreen`, `CreateIntroFMVScreen`,
  `CreatePSAScreen`/`CreatePSAMovieScreen`, `CreatePlayerPressStart`,
  `CreateLoadingScreen`/`CreateLoadingControllerScreen`/`CreateLoadingTipsScreen`,
  `CreateDiscErrorScreen`, `CreateMemCardBootScreen`,
  `CreateMemcardMainMenu`/`CreateMemcardListFiles`/`CreateMemcardNullScreen`.
- **Main menu / career hub**: `CreateMainMenu`, `CreateCareerCribMenu`,
  `CreateCareerCarLotMenu`, `CreateCareerStatisticsScreen`,
  `CreateCareerStatusScreen`, `CreateCribCarSelect`,
  `CreateCribRewardOptionsMain`, `CreateCribReward_Sponsors`,
  `CreateCribWorldMapScreen`, `CreateInGameWorldMapScreen`.
- **Garage / customization**: `CreateGarageMainScreen`,
  `CreateBuyPerformancePartsScreen`, `CreatePartSelectionScreen`,
  `CreateChoosePerfCategoryScreen`/`CreateChoosePerformancePackagesScreen`,
  `CreateChooseCustomizeCategory`, `CreateChooseBrandsScreen`,
  `CreateChoosePaintScreen`/`CreateChoosePaintShopCategoryScreen`,
  `CreateChooseVinylLayerScreen`, `CreateDecalCategoryScreen`/
  `CreateDecalsOverlayScreen`, `CreateChooseRimBrand`/
  `CreateChooseSpinnerBrandScreen`, `CreateChooseIceCategory`/
  `CreateChooseIcePartsBrowser`, `CreateNeonPartsBrowser`/
  `CreateCustomizeNeonMenu`, `CreateChooseTuningCategoryScreen`/
  `CreateChooseTuningSettingScreen`, `CreateCarTunePauseScreen`,
  `CreateDynoRunChartScreen`/`CreateDynoRunResultsScreen`,
  `CreateHUD_Customizer`/`CreateCustomHUD_Browser`,
  `CreateDebugCarCustomize` (debug-only).
- **Post-race**: `CreatePostRaceMenuScreen`, `CreatePostRaceResultsScreen`,
  `CreatePostRaceRewardsScreen`, `CreatePostRaceStatsScreen`,
  `CreatePostRaceSponsorsScreen`/`CreatePostRaceSponsorsPopup`,
  `CreatePostRaceTournamentResults`/`CreatePostRaceTournamentStandings`.
- **Options/pause**: `CreateOptionsMainScreen`/`CreateOptionsScreen`/
  `CreateOptionsControllerScreen`, `CreatePauseMenu`/
  `CreatePauseOptionsMainScreen`/`CreatePauseOptionsScreen`.
- **Magazine/rewards misc**: `CreateMagazineSelect`/`CreateMagazineView`/
  `CreateMagazineBack`/`CreateMagazineReward` (+ `MagazineViewer` class,
  `EnterFrontendMagazineReward`/`EnterFrontendDvdReward`), `CreateChyronScreen`,
  `CreateEaTraxScreen` (soundtrack browser), `CreateCreditsScreen`,
  `CreateInGameMoviePlayScreen`.
- **Misc/system**: `CreateFEKeyboard`, `CreateEventEngageDialog`,
  `CreateDebugTestScreen`/`CreateDVDStatisticsScreen` (debug-only).

## Sound/event system

A parallel `FrontendSoundEvent`/`FrontendSoundTrigger`/`eMenuSoundTriggers`
system exists (`MakeFrontendSound`, `FilterSoundForMenu`/
`FilterSoundForMenuAccept`/`FilterSoundForMenuBack`/
`FilterSoundForHorizontalMenu`/`FilterSoundForBrowser`/
`FilterSoundForMovieViewer`, `InitFrontendSounds`, `FrontendSoundMap` global,
`LoadFrontEndSoundBanks`). Menu navigation sounds are driven by this, not
ad-hoc per-widget calls -- relevant once audio porting starts (this ties into
`3DS Files` audio work, currently not started).

## Data/resource side (not yet investigated)

Not yet determined: where screen *layout* (widget positions, text/labels,
background art references) lives -- almost certainly **not** hardcoded in
the `Create*Screen` C++ functions themselves (that would be unusual for a
game this size with 80+ screens), more likely a resource format loaded at
runtime. `FrontEndMemoryFile` (a `D` symbol, i.e. a data global) is a
promising lead -- likely a preloaded blob/pak the frontend streams its
resources out of. Next step: find what loads `FrontEndMemoryFile` and what
container format it parses (may reuse the same `.BIN`/`.BUN` chunk format
already documented in `docs/formats/` for tracks/cars, or may be a distinct
frontend-specific pack -- needs checking against the PC `PC Files/FRONTEND*`
or similarly-named directory if one exists).

## `FrontEndMemoryFile` / `FrontB.lzc` decoded (session 12)

`PC Files/GLOBAL/FrontEndMemoryFile.bin` is only 16 bytes (a small header/
pointer stub, not the real data). The actual frontend resource pack is
**`PC Files/FRONTEND/FrontB.lzc`** (1,750,771 bytes) -- and it uses the
*same* `JDLZ` compression already reverse-engineered for car/track assets
(`tools/jdlz.py`). It decompressed cleanly with the existing tool, no new
compression format needed here:

```python
sys.path.insert(0, "tools"); import jdlz
data = open("PC Files/FRONTEND/FrontB.lzc", "rb").read()
out, complen = jdlz.decompress(data, 0)   # complen == len(data), one block
# len(out) == 5,969,360 bytes decompressed
```

(`FRONTEND/FRONTA.BUN` is 0 bytes in this install -- either genuinely empty
in retail or a companion file not populated; `FrontB.lzc` alone holds real
data. `FRONTEND/MAGAZINES_FRONTEND.BIN`, 7700 bytes, is a separate smaller
resource, not yet inspected.)

The decompressed 5.97MB blob starts with the debug path string
`Global\Pipeline\PC\eLabFrontEnd.bin` (confirms this is the compiled output
of the `eLabFrontEnd.bin` pipeline step referenced by the `FrontEndMemoryFile`
symbol), then is dominated by **thousands of resource ID strings** -- e.g.
`MAIN_ICON_CAREER`, `CARSELECT_MANUFACTURER_*`, `SECONDARY_LOGO_ECLIPSE`,
`VISUAL_PART_HOOD`, `DECAL_ICON_*` (dozens of aftermarket brand decals),
`VINYL_GROUP_*`/`VINYL_LAYER_1..4`, `TRACK_ICON_SHORT_TRACK`, `LANG_SPANISH`/
`LANG_ITALIAN`/`LANG_CHINESE`/etc. (localization variants), `OPTIONS_ICON_*`,
`ONLINE_ICON_*`, `CRIB_ICON_*`, `SFX_SMOKE_B_CS`, plus debug-only markers
(`DEBUG_CYLINDER`, `MARKER_GREENARROW`, `MARKER_GPS_AID`, etc. -- these read
like shared engine-wide marker names, not frontend-specific, so this resource
table may not be 100% frontend-exclusive). One explicit texture-package
reference was found inline: **`Global\FrontEndTextures.tpk`** -- this is the
same `.tpk` texture-package concept already documented in
`docs/formats/texture-tpk.md` for car/track textures, strongly suggesting all
menu icon/logo/background art goes through that same texture pipeline.

No file named `FrontEndTextures.tpk` exists directly under `PC Files/` --
like other packages in this project, it was almost certainly compiled into
one of the `GLOBAL/` container files by build time. `PC Files/GLOBAL/`
contains promising untouched candidates: `GlobalTexturesNA.bin` /
`GlobalTexturesROW.bin` (44,448 bytes each -- likely small index/manifest
files given the size, paralleling the `0x33310003` per-texture offset table
pattern from `texture-tpk.md`), and the much larger `GLOBALB.BUN` (8MB) /
`GlobalB.lzc` (5.1MB, also JDLZ-compressed almost certainly) which likely
hold the actual frontend texture pixel data as named leaves.

This resource-ID string table is very likely a **name/lookup index**
(string ID -> texture or layout resource) rather than the widget layout data
itself -- the actual per-screen widget positions/hierarchy are still
unlocated. Next step per the plan above: find what code parses this blob
(search `Speed.elf`/PC exe for references to these string literals, e.g.
`"MAIN_ICON_CAREER"`) to learn the surrounding record structure, which should
reveal how these names key into layout/texture data.

## Decompiled `CreateLegalScreen` chain (session 13 continuation)

Used Ghidra headless (`analyzeHeadless.bat` against the existing `NFSU2.gpr` project, which already
has both `SPEED2.EXE` and `Speed.elf` imported at `/`) with a small custom `DecompileFunc.java`
post-script (`DecompInterface` + `decompileFunction`, output written to a text file) to decompile
functions by address without needing the Ghidra GUI open. This is the repeatable pattern going
forward -- edit the address list in the script, rerun, read the output file.

**`CreateLegalScreen` (0x8010412c)** is a thin factory, as expected from the naming convention:
allocates a `DemoLegalScreen` (0x58 bytes) and calls its constructor. Nothing screen-specific here.

**`DemoLegalScreen::DemoLegalScreen` (0x80103ed4)** -- calls the `MenuScreen` base constructor, then:
```c
FEngFindString(screenHandle, 0x4b98c4b9);      // look up a named FE element by hash
FEngSetLanguageHash(elem, 0xe90f40a4);          // bind it to a localized text string (by hash)
FEngFindString(screenHandle, 0x94dce36a);       // a second named element
FEngSetLanguageHash(elem2, 0x96b89c4c);
FEngSetScript(screenHandle, 0x4b98c4b9, "ANIMATE_1", true);
FEngSetScript(screenHandle, 0x94dce36a, "ANIMATE_2", true);
```
**Key architectural finding: the widget tree and layout are NOT built in C++.** `FEngFindString`
(0x8014faac) just calls a generic `FEngFindObject` and type-checks the result -- it looks up an
already-existing named object by hash, it doesn't construct one. So the actual widgets (text boxes,
logos, etc.) for a screen must already exist, loaded from an external per-screen resource, by the time
this constructor runs -- this C++ code only *wires up* screen-specific behavior (which localized text
goes in which slot, which named animation script plays) on top of a pre-loaded, data-defined layout.

**`DemoLegalScreen::NotificationMessage` (0x80103f94)** handles one message type (`-0x367ca946`,
likely a per-frame/timer tick) implementing a real "legal screen" state machine: play `"ANIMATE_1_GO"`
once, then after a timeout play `"ANIMATE_2_GO"` and call `ChangeToNextBootFlowScreen`. So each screen
class's job is mostly: react to input/timer notifications and trigger named animation scripts /
advance the flow -- confirms `NotificationMessage` is the per-screen input/event handler entry point
seen across the whole `NotificationMessage__<ScreenClass>...` symbol family from the earlier survey.

**`MenuScreen::MenuScreen` (0x801515a0)** (the common base most screens share) does bookkeeping (player
cash display via `SetAlternatingPlayerCash`, or profile name via `FEngPrintf`+`GetProfileName`) and,
notably, branches on **`.fng` filenames** via `bStrCmp`: `"UI_OLEAMessenger.fng"`, `"GarageMain.fng"`,
`"Keyboard.fng"`, `"UI_VirtualKeyboard.fng"` are special-cased. **`.fng` is the real per-screen
resource/package file extension** (name plausibly "FrontEnd" + something, exact meaning not yet
confirmed) -- this is the thing to decode, not the C++ classes, to recover actual screen layouts.

**`ChangeToNextBootFlowScreen` (0x801035c4)** confirms a **`BootFlowManager`** class drives the
intro sequence as an ordered list of named screens: `MaybeMoveForward__15BootFlowManager` /
`AtLastScreen__15BootFlowManager` step through it, and `FEngSwitchPackages(..., screenName, 0)` is
the actual "load and switch to this named .fng package" call. Two names appear as special-cased
targets here too: `"MC_Background.fng"` (memory-card check gate) and `"UI_Main.fng"` (falls through to
starting career free-roam instead of switching packages, i.e. `UI_Main.fng` is confirmed as **the real
main menu's package name**).

### `.fng` package names recovered directly from game data

No standalone `.fng` files exist on disk (same pattern as other formats in this project -- compiled
into a container at build time). But the already-decompressed `PC Files/FRONTEND/FrontB.lzc` (see
above) contains a **name table listing at least 80 real `.fng` package names** -- this is the actual
screen registry, in the game's own data, not reconstructed from guesswork. Confirmed real names include
(non-exhaustive): `UI_Main.fng` (main menu), `GarageMain.fng`, `UI_Options.fng`,
`UI_Options_PC_Controller.fng`, `UI_PC_Customize_Options.fng`, `UI_Wheel_Options.fng`,
`UI_CribRewardOptionsMain.fng`, `UI_CareerSelect.fng`, `UI_CareerWorldMap.fng`,
`UI_MagazineReward.fng`/`UI_MagazineSelect.fng`/`UI_MagazineView.fng`, `UI_RewardsSponsor.fng`,
`UI_Menu_Asset_Reputation.fng`, `UI_VirtualKeyboard.fng`, and a `GenericDialog_{SMALL,MED,LARGE}.fng`
/ `GenericDialog_Animate_{SMALL,MED}.fng` family (shared popup-dialog templates reused across screens).

Note on encoding: these names are stored **chunked into 8-byte-aligned fixed-width slots** -- e.g.
`UI_CribRewardOptionsMain.fng` appears in the raw bytes as four consecutive 8-byte pieces
(`UI_Crib\0`, `RewardOp`, `tionsMai`, `n.fng` + pad) rather than one contiguous null-terminated C
string. This is very likely a hash-table/string-interning scheme (8-byte chunks are convenient for
fast chunked comparison/hashing) rather than a bug in how the string was read -- worth confirming once
the surrounding record structure (a few bytes with values like `0x03 0x00`, `0x1d 0x03 0x00` immediately
before each name, possibly type tags, lengths, or an entry's hash/offset fields) is decoded.

## `.fng` / `FEPackage` chunk container format decoded (session 13, continued)

`FEngSwitchPackages` (0x8014f4d8) turned out to be a thin wrapper around
`Switcher::5cFEng` -- it queues a package switch on the global `cFEng`
render/engine singleton rather than parsing bytes itself. `FEngFindObject`
(0x8014fa3c) is the real payoff: it calls `FEngFindPackage` to get a live
`FEPackage*` by name, then `FindObjectByHash__9FEPackageUl(pkg, hash)`, falling
back to a global engine object cache (`FindObject__C7FEngineUlPCc`) if the
named package isn't loaded/found. `FindObjectByHash` itself just runs a
`ForAllObjects` callback (`FEFindByHash`) over the package's object list
comparing hashes -- so objects are **not** indexed by hash at load time, they're
loaded into a flat/tree object list and hash-matched linearly on lookup.

The real find was following `FEPackage`'s loader, `FEPackageReader`
(`Load__15FEPackageReader` @ 0x8013a3cc and its chunk-reader helpers). This is
the actual `.fng` binary parser and it fully reveals the container format:

**Generic chunk format** -- every chunk is `[FourCC:4][Length:4][Payload]`,
stored **little-endian on disk** even though the GameCube binary is
big-endian (all fields are byte-swapped after reading, confirmed directly in
the decompiled code, e.g. `(uVar2>>0x18 | uVar2<<0x18 | (uVar2&0xff00)<<8 |
uVar2>>8&0xff00)` is a 32-bit byte-swap applied to every multi-byte field).
Siblings are found by `FindChild` walking `payload_start + length` repeatedly
until a matching FourCC is found or the parent's end is reached -- a plain
linear scan, not an offset table.

**FourCC tags identified** (4 ASCII bytes as stored in the file, left to
right):
- `PkHd` -- Package Header (root chunk: contains package flags/checksum at
  offset 0x18/0x1C, then a `Name` C-string, then an offset to a `Filename`
  C-string measured from the start of `Name`)
- `TypS` -- Type Sizes table (a shift-right-3 count field, i.e. a table of
  N/8 entries -- likely per-object-type record-size table)
- `Res\xCC` -- Resources chunk (confirmed in real files, e.g.
  `GenericDialog_SMALL.fng` has one right after `TypS`): contains `RsNm`
  (Resource Name) sub-chunks, each holding a description string like
  `(Untextured Simple Image)` followed by a real texture filename like
  `U2_Menu_Corner_white.t...` -- this is the per-package list of texture/font
  resources the screen references. Referenced-packages and library-refs
  chunks (`ReadReferencedPackagesChunk`, `ReadLibraryRefsChunk`) are declared
  in the loader but weren't present in the two files inspected so far
  (optional/empty when a screen has no cross-package refs) -- still not
  decompiled or named.
- `PkgR` -- Package Response chunk (confirmed in real files, follows the
  object list) -- not yet decompiled (`ReadPackageResponseChunk`).
- `Targ` -- Message Target List chunk (confirmed in real files, last chunk
  before `Startup`) -- not yet decompiled (`ReadMessageTargetListChunk`).
- an object-list container (FourCC bytes `4f 62 6a` + `0xCC`, i.e. `"Obj"` +
  a non-ASCII control byte -- used as a *list* wrapper, distinct from a single
  entry) holding a sequence of per-object chunks
- `ObjD` -- one Object Data entry inside the object-list container (this is a
  single widget/FEObject's serialized record -- not yet decompiled further,
  but `ReadObjectTags__15FEPackageReaderP5FETagUl` is the function that parses
  its interior, and is the next concrete decode target for widget layout)
- `Butn` -- a button-map count embedded directly in the object-list stream
  (calls `SetCount__11FEButtonMapUl`)
- `MsgR` -- Message Responses (per-object message/response table, parsed by
  `ReadMessageResponseTags`)
- `Scrp` -- Scripts (per-object named animation scripts, e.g. `ANIMATE_1`
  /`ANIMATE_1_GO`, parsed by `ReadScriptTags`)
- `FOb\xEA` -- **correction, confirmed against real bytes**: this is not a
  terminator, it's the **per-object envelope** wrapping one widget's data --
  the object-list container (`Obj\xCC`) is just a flat sequence of `FOb\xEA`
  entries (60 of them in `GenericDialog_SMALL.fng`), and each `FOb\xEA`'s
  payload is itself a nested chunk stream containing exactly one `ObjD`
  (the widget's `Ot`/`OP`/`PA`/`SA`/`On`/`Oh` tag stream, decoded below) plus
  zero or more `Scrp` (named scripts) and `MsgR` (message responses) chunks
  for that same object. Verified byte-for-byte: `FOb\xEA` len 580 -> contains
  `ObjD` len 108, four `Scrp` chunks, one `MsgR` chunk, each already a valid
  self-contained FourCC+length chunk per the format above.
- the outermost file-level wrapper chunk has FourCC bytes `46 45 6e` + `0xE7`
  (`"FEn"` + control byte) before the `PkHd` chunk -- this is almost certainly
  the top-level "FEng chunk list" container tag for the whole file, i.e. every
  `.fng` file's first 4 bytes on disk should be `46 45 6E E7`.

**Load order** (from `Load__15FEPackageReader`, matches file layout top to
bottom): Header -> TypeSizes -> ReferencedPackages -> LibraryRefs ->
Resources -> Objects -> PackageResponse -> MessageTargetList -> then
`Startup__9FEPackage` runs.

**Chunk length semantics (confirmed against real bytes, see next section):**
the 4-byte length field is the **payload size only** (it excludes the 8-byte
tag+length header itself), and `FindChild`'s sibling-search step is
`next_chunk_offset = this_chunk_payload_start + this_chunk_length` where
`this_chunk_payload_start = this_chunk_tag_offset + 8`. E.g. a `PkHd` chunk
with length `0x5c` (92) starting at file offset 0 has its tag at 0, length
field at 4, payload from 8 to 8+92=100 -- so the next sibling chunk (`TypS`)
starts at exactly offset 100. This was verified byte-for-byte against a real
extracted `.fng` file (below), not inferred from decompilation alone.

## Real `.fng` files extracted and format-verified (session 13, continued further)

The 80+ names found in `FrontB.lzc` are not just bare strings -- **every real
`.fng` screen file is embedded whole in `FrontB.lzc`**, individually
compressed. `FrontB.lzc` is one big outer JDLZ block (see `tools/jdlz.py`)
whose decompressed ~5.97MB payload contains **130 separate nested JDLZ
sub-blocks** back-to-back, each of which decompresses to one complete,
self-contained `.fng`/`FEPackage` chunk stream starting with the exact magic
`46 45 6E E7` predicted above, immediately followed by a `PkHd` chunk whose
payload holds the real Name (e.g. `LS_Demo_Legal.fng`) and original build
path Filename (e.g. `Global\FEng_Screens\Stripped\LS_Demo_Legal.fng`).

Wrote `tools/fng_extract.py` (same style/location as `jdlz.py`,
`dxt_transcode.py`) to pull all 130 out by their real names:

```bash
python tools/fng_extract.py "PC Files/FRONTEND/FrontB.lzc" extracted/fng
```

This produced all 130 real screen files in `extracted/fng/`, confirmed
byte-correct for `LS_Demo_Legal.fng` (9292 bytes, matches the `DemoLegalScreen`
class already decompiled above) and `GenericDialog_SMALL.fng` (18112 bytes,
the planned first full-decode target). The `PkHd` header of
`LS_Demo_Legal.fng` was manually walked byte-by-byte and matches the format
above exactly: magic, length=0x2444, `PkHd` tag, length=0x5c, a `0x00020000`
version-stamp field (this, not a size check, is what `ReadHeaderChunk`'s
`< 0x20000` comparison actually validates -- every real file has exactly this
value), two flag/checksum u32s, a name-to-filename byte offset, `Name`
C-string, then `Filename` C-string. `TypS` immediately follows at the
predicted offset with entries `(type=5/Group, size=0x44)`,
`(type=2/String, size=0x44)`, `(type=1/Image, size=0x54)` -- i.e. this also
pins down the live in-memory object struct sizes per type (68/68/84 bytes).

This is a real extracted-asset milestone, not just documentation: 130 actual
game screens (menus, dialogs, HUD overlays, career/garage/customize/online
screens, the full multiplayer/memory-card/profile UI, `Credits.fng`, etc.)
now exist as real files on disk, ready for the `ObjD`/widget-tag parser
(format decoded above) once written.

This is a real, fully generic, self-describing chunk format (conceptually
like IFF/RIFF but with swapped-endian words and 3-letter-tag+control-byte
FourCCs) -- exactly what's needed to write a real `.fng` parser/dumper tool
(alongside `jdlz.py`/`dxt_transcode.py`) once the `ObjD` interior fields are
decoded. No widget/menu C++ code needs to be written by us at all for layout
-- the real per-screen visual layout is 100% data in these chunks, to be
parsed and rendered by a ported `FEPackage`/`FEObject` runtime, not
recreated.

## Per-widget "ObjD" tag format decoded (session 13, continued further)

`ReadObjectTags__15FEPackageReaderP5FETagUl` (0x8013b584) parses the interior
of each `ObjD` entry, and it's a **different, smaller TLV scheme** than the
outer 4-byte-FourCC chunk format: each tag record is a **4-byte header**
(2-byte tag + 2-byte length, both byte-swapped same as everywhere else)
followed by `length` bytes of payload, and the reader just walks forward
`4 + length` bytes per record until it exhausts the `ObjD` entry's total byte
range. Confirmed tags (2 ASCII chars each, in on-disk order):

- **`Ot`** -- Object Type: payload is a type ID (u32) passed straight to
  `CreateObject__15FEPackageReaderUl`, which allocates the concrete widget of
  that type and becomes the "current object" for all following tags until the
  next `Ot`. This is the tag that starts a new widget inside the object list.
  The type ID also drives which type-specific tag processor later tags get
  routed to (see below) via the live object's vtable-type field at offset
  0x18: `1`=Image, `2`=String/Text, `4`=ListBox, `5`=Group, `6`=CodeListBox,
  and `9`/`10` also route through the Image handler (icon-bearing widgets
  like buttons/sliders reusing image tags).
- **`OP`** -- Object Properties: 4 fields -- `x`(u32 @ +0xc), `y`(u32 @ +0x10),
  `flags`(u32 @ +0x1c), and a `u16 @ +0x22` (likely a sub-type/variant or
  render-order value). Two flag bits are handled specially inline:
  - flag `0x100000` -- "inherits from a referenced/template object": calls
    `FindReferencedObject`, and if found, **replaces** the current object with
    either a copy (`FEGroup` copy-ctor if the template's type==5/Group) or a
    freshly-constructed same-type object, then copies pos/flags/extra fields
    across. This is exactly the "reused template" mechanism behind
    `GenericDialog_SMALL.fng`-style shared widgets.
  - flag `0x10000000` -- "is a button": appended into the package's button
    map array (`FEButtonMap`, sized earlier by the `Butn` count tag).
- **`PA`** -- Parent: payload is a GUID (u32), resolved via
  `FindObjectByGUID__9FEPackageUl` to set the current object's parent link.
- **`SA`** -- an inline data-array blob: payload is copied word-by-word
  (byte-swapped) into a buffer already owned by the object at `+0x2c` --
  likely per-vertex/keyframe or list-cell numeric data, not yet pinned down
  precisely.
- **`On`** -- Object Name: payload is a C-string, passed to
  `SetName__8FEObjectPCc` (only when the reader is in "named" mode,
  `param_1[3]!=0` -- so not every object gets a name, only debug/lookup-able
  ones, matching how `FEngFindObject` name-hash lookups target specific named
  objects, not every widget).
- **`Oh`** -- a single-field override for `y` (`+0x10`), independent of `OP`
  (likely used by template-instance tags to override just one inherited
  coordinate).

Type-specific tags (dispatched once the current object's type is known):

*String/Text objects (type 2, `ProcessStringTag`, all fields on the live
`FEString`/`FEWideString`)*:
- **`SH`** -- language Hash key (@ +0x60) -- this is the *exact* same field
  `FEngSetLanguageHash` (0x8014fb1c) writes at runtime, confirming hash-driven
  localized strings are set from this on-disk tag by default and can be
  overridden in code.
- **`SL`** -- literal Label text (only applied when the reader is in a mode
  that allows literal/non-localized strings, `param_1[0xc]!=0`).
- **`St`** -- the actual wide-string text payload; also runs a manual
  byte-swap over every UTF-16 code unit after assignment (confirms in-file
  wide strings are little-endian UTF-16, converted to big-endian PPC native
  after load).
- **`Sb`** -- wide string buffer/length allocation (@ offset+100, the
  `FEWideString` sub-object).
- **`Sl`** -- a length-related field (@ +0x70, possibly max length/truncation).
- **`Sw`** -- a field @ +0x74 (possibly wrap width or line count).
- **`Sj`** -- a field @ +0x6c (possibly justification/alignment, distinct from
  the CodeListBox `Lj` tag below despite similar naming).

*Image objects (type 1/9/10, `ProcessImageTag`)*:
- **`IF`** -- Image/texture reference: a single hash/ID field (@ +0x5c) --
  this is the texture-name-hash link into the texture package (see
  `Global\FrontEndTextures.tpk` from the earlier session's findings).

*CodeListBox objects (type 6, `ProcessCodeListBoxTag`)*:
- **`LD`** -- Initialize(rows, cols) -- grid dimensions.
- **`SI`** -- AllocateStrings(count, ...) -- per-cell string storage.
- **`Lj`** -- SetCellJustification.
- **`Cc`** -- SetCellColor.
- **`Cs`** -- SetCellScale.
- **`Lt`** -- a packed flags/bit toggle (@ +0x68).
- **`Lv`** -- two fields (@ +0x84/+0x88), likely scroll/view offset or
  velocity.

This is enough to write a real, working `.fng`/`ObjD` parser now that real
file bytes are extracted (see below) -- the tag set above covers Image,
Text, and List/CodeListBox widgets, which is the large majority of what
`UI_Main.fng`-style screens are built from.

**Validated against a real object** (`GenericDialog_SMALL.fng`, first `FOb\xEA`
entry's `ObjD` payload, 108 bytes, walked by hand as 4-byte-tag(2 ASCII) +
2-byte-length(u16) + payload records -- note the *inner* `ObjD` tag records
use a 2-byte tag + 2-byte length header, half the size of the outer FourCC
chunk headers):
```
Ot  len=4   -> 00000005            (object type = 5 = Group, per TypS table)
Oh  len=4   -> 12da98fb            (y override, raw float bits)
OP  len=16  -> x=0x0defe6, y=0x12da98fb, flags=0x00000040, extra_u16=0x0000
SA  len=68  -> 17 x u32 (mostly 0xff/0x00 bytes + float-looking tail incl.
               0x3f800000 = 1.0 repeated 4x -- likely an RGBA-per-corner
               color array plus a uniform-scale/UV tail for a Group)
```
This matches the decompiled `ReadObjectTags` logic exactly (tag order,
lengths, and which struct offsets each tag would write) with zero
discrepancies -- the format write-up above is not speculative, it's
confirmed against real shipped data.

## `tools/fng_parse.py` -- automated parser/dumper (session 14)

Wrote `tools/fng_parse.py`, implementing the full chunk tree + `ObjD` tag
format documented above, and ran it against all three planned validation
targets:

```bash
python tools/fng_parse.py extracted/fng/GenericDialog_SMALL.fng extracted/fng_parsed/GenericDialog_SMALL.json
python tools/fng_parse.py extracted/fng/LS_Demo_Legal.fng       extracted/fng_parsed/LS_Demo_Legal.json
python tools/fng_parse.py extracted/fng/UI_Main.fng             extracted/fng_parsed/UI_Main.json
```

Results, cross-checked against the hand-walked bytes above:

- `TypS` entries decode to exactly `(Group, 68)`, `(Image, 84)`, `(String,
  68)` for `GenericDialog_SMALL.fng` -- matches the earlier hand-decode
  (0x44/0x54/0x44) exactly.
- `PkHd`'s **actual field layout differs slightly from the earlier
  hand-walk guess** and was corrected against real bytes: a fixed 24-byte
  header (`+0`=version stamp `0x00020000`, `+4/+8/+12`=three fields not yet
  semantically pinned down -- values `0`, `12`, `9` for
  `GenericDialog_SMALL.fng`, `0`/`14`/`19` for `UI_Main.fng`, so likely
  per-file counts of some kind, not flags/checksum as originally guessed),
  then the `Name` C-string, then the `Filename` C-string immediately after
  `Name`'s NUL. This doesn't change chunk-tree walking (siblings are found
  by declared chunk length regardless of internal field meaning) but
  corrects the header's own field semantics.
- `Res\xCC`/`RsNm` resource lists decode cleanly, e.g. `UI_Main.fng`
  references real texture files `U2_Menu_logo.tga` (the actual NFSU2 logo),
  `U2_Menu_Arrow.tga`, `U2_CarSelect_icon_backing.tga`,
  `U2_Icon_Highlight.tga`, `U2_Scroller_Tab.tga`, plus the
  `ConduitMdITC_TT21i.ffn` menu font -- consistent with a real main-menu
  layout, not placeholder data.
- `UI_Main.fng` parses to 55 widgets; its `String`-type widgets all carry an
  `SH` language hash (e.g. `2283769594`, `3631297468`, ...) with **no**
  literal (`SL`) fallback text -- confirming the real main menu's visible
  text is 100% localized/hash-driven, resolved at runtime against a
  per-language string table (`PC Files/LANGUAGES/English.bin` etc.), not
  baked into the `.fng` file itself. `English.bin` has its own
  header+table structure (starts `00 90 03 00 30 D2 03 00 ...`, i.e. two
  large offsets into the 270KB file) that has **not yet been decoded** --
  this is why no literal legal/menu text could be surfaced yet for the
  boot-sequence work below; resolving it is the concrete next step for
  getting real on-screen text.
- `OP`'s exact per-field bit/flag semantics (the `0x100000`/`0x10000000`
  flag checks from the decompile) still don't cleanly match the raw x/y/
  flags word values pulled directly from real bytes (e.g. `GenericDialog_
  SMALL.fng`'s first widget: raw `x=0x000def6e`, `y`-word bit pattern
  matching a float, `flags`-word `0x40000000`) -- likely either a
  fixed-point/float interpretation for x/y (not integers) or an
  endian-order subtlety not yet nailed down precisely. Not blocking (the
  parser stores raw values either way), but flagged here as unresolved.

Outputs are saved at `extracted/fng_parsed/*.json` for the three validation
targets. This is the first real, structured, machine-readable dump of an
actual NFSU2 screen layout (`UI_Main.fng` = the real main menu) -- task #6
(building the real menu) can eventually consume this JSON directly instead
of hand-parsing `.fng` bytes, once localized string resolution and texture
loading are also in place.

## Remaining chunk-content readers decompiled (session 24, 2026-08-30)

Ran the same Ghidra headless recipe (`tools/ghidra_scripts/DecompileFunc.java`, now committed) against
the 5 addresses task #49 called out. All decompiled cleanly against `Speed.elf`. Tag FourCCs are
confirmed by the same convention established earlier: the on-disk magic constant's bytes, read
MSB-first out of the decompiled hex literal, spell the FourCC *reversed* (e.g. `0xcc736552` -> bytes
`CC,73,65,52` -> reversed `52,65,73,CC` = `"Res\xCC"`).

- **`ReadResourceChunk`** (`8013ab50`) -- confirms and completes the `Res\xCC` reader from session 13.
  Finds a nested `RsNm` (`0x6d4e7352`) child, then a `RsRq` (`0x71527352`) sibling immediately after it
  (found by raw pointer arithmetic, not `FindChild`) holding the real **resource count** and a table of
  fixed **24-byte records**: `{u32 field0, u32 nameOffset, u32 field2, u32 field3, u32 field4, u32
  field5}`. `field0`'s semantics not yet pinned (first word of each record); `field2..field5` likewise
  unlabeled -- likely resource type/hash/dimensions given `RsNm`'s payload is texture/font names (see
  session 13). After the record table, a raw string blob (the resource *names*, i.e. `RsNm`'s own
  string data) is copied out and each record's `nameOffset` is relocated in-place into a real pointer
  into that blob (`record[1] += stringBlobBase`). This is the piece needed to resolve an `IF` (Image
  tag)'s texture-name-hash into an actual filename string at parse time.
- **`ReadPackageResponseChunk`** (`8013ae54`, tag `PkgR` = `0x52676b50`) -- thin: just calls the same
  `ReadMessageResponseTags` used for per-object `MsgR` chunks, with a boolean flag (`true`) presumably
  distinguishing "package-level" vs "object-level" response registration. No new record format --
  `MsgR`'s tag reader (not yet separately decompiled) is the real target if response *content* is
  needed later.
- **`ReadMessageTargetListChunk`** (`8013cf34`, tag `Targ` = `0x67726154`) -- an inner 2-byte-tag +
  2-byte-len TLV stream (same shape as `ObjD`'s tags), with two subtags: **`Tc`** (`0x6354`, target-list
  count -- allocates an array of `FEMsgTargetList` structs, `count*16+8` bytes) and **`Mt`** (`0x744d`,
  one target-list entry -- a message ID (u32) followed by a run of GUIDs, each resolved via
  `FindObjectByGUID` and appended into that message's target list via `AppendTarget`). This is the
  "which objects get notified when message X fires" table.
- **`ReadReferencedPackagesChunk`** (`8013a858`) -- **tag is actually `LibL` (`0x4c62694c`), not a
  bespoke "referenced packages" tag** as the docs had guessed pre-decompile; the content matches:  a
  list of package-name strings, each wrapped in an `FENode` and linked into the package's own min-list
  (`+0x80`/`+0x88`), then a second pass walks that same list calling `FindLibraryPackage` /
  `LoadPackage` / `AddToLibraryList` on the engine singleton for each -- i.e. this chunk is what
  actually triggers loading of cross-referenced sibling `.fng` packages (shared templates like the
  `GenericDialog_*.fng` family are almost certainly pulled in through this, not just referenced by
  name).
- **`ReadLibraryRefsChunk`** (`8013aa14`, tag `LibR` = `0x5262694c`) -- straightforward: fixed 12-byte
  records (3x u32), record count = `payload_len / 12` (validated by exact division, returns `false`
  otherwise), copied into the package's own ref-table array (`SetNumLibraryRefs` + a raw copy loop into
  `+0x94`). Fields not individually labeled yet, but shape (3 words/record) suggests
  `{sourcePackageIndex, localGUID, remoteGUID}` or similar cross-package object linkage -- would need a
  real multi-package `.fng` sample with a nonzero `LibR` chunk to confirm by inspection.

**Net effect: the full `.fng`/`FEPackageReader` container spec is now decompiled end-to-end** -- every
chunk type declared in `Load__15FEPackageReader`'s load order (Header -> TypeSizes ->
ReferencedPackages(`LibL`) -> LibraryRefs(`LibR`) -> Resources(`Res\xCC`/`RsNm`/`RsRq`) ->
Objects(`Obj\xCC`/`FOb\xEA`/`ObjD`/`Scrp`/`MsgR`) -> PackageResponse(`PkgR`) ->
MessageTargetList(`Targ`)) has a known record layout. Remaining unknowns are all field-level (a few
unlabeled `u32`s per record), not structural -- `tools/fng_parse.py` can be extended to decode all of
these instead of stopping at Objects/Resources/TypeSizes as it does today, once someone needs the
extra chunks' data for actual gameplay (library-package loading, message-target dispatch). For the
immediate goal of rendering `UI_Main.fng`'s visible widgets, `Res\xCC`/`RsNm` (texture name resolution)
is the only one of these five that's on the critical path; `PkgR`/`Targ`/`LibL`/`LibR` matter once
button-press message routing and shared-template package loading are implemented.

## `ObjD` tag semantics decoded via `ReadObjectTags`/`Process*Tag` (session 24 part 3, resuming task #50)

Decompiled `ReadObjectTags__15FEPackageReaderP5FETagUl` (`8013b584`, the interior TLV walker for one
`ObjD` entry) plus its per-type dispatch targets `ProcessImageTag`/`ProcessStringTag`/
`ProcessListBoxTag` (`8013bc68`/`8013ba78`/`8013bcac`). Findings, several of which **correct earlier
guesses** in `tools/fng_parse.py`:

- **The real Image texture-hash tag is `"If"` (mixed case), not `"IF"`.** `fng_parse.py`'s parser had an
  uppercase-only check that never matched real files (confirmed empirically: real `.fng` bytes use tag
  `If`), meaning `textureHash` was **never actually being extracted** in any prior session's output
  despite the field existing in the JSON schema. Fixed this session. `ProcessImageTag` confirms the
  4-byte payload is written to the live `FEObject`'s `+0x5c` field.
- **Scanned all 130 files in `extracted/fng/` (session 24 part 3): 7881 total `If` tags found, every
  single one is `0x00000000`.** This is not a `UI_Main.fng`-specific quirk — **the on-disk `If` field is
  never actually used to store a texture hash in any shipped screen file.** This means **texture
  assignment for real menu images does not happen via a per-widget `If` hash at load time, full stop**
  — the mechanism is genuinely something else. Possibilities not yet investigated: textures assigned
  dynamically via the per-object `Scrp` script/animation system (parsed but not decoded — likely
  candidate given the pattern of `ANIMATE_1`/`ANIMATE_1_GO`-style named scripts already observed), via
  the shared-template `GenericDialog_*.fng` library-ref mechanism (`LibL`/`LibR`, still unparsed), or via
  `RenderImage__11cFEngRenderP7FEImageP14FERenderObjectP19FEPackageRenderInfo` (not yet decompiled)
  resolving a texture through some other live-object field entirely unrelated to what `ReadObjectTags`
  writes at load time. **This is the actual remaining blocker for #50's texture pipeline**, not the
  `Res\xCC`/`RsRq` record format (which is fully decoded, see below, and correctly resolves texture
  *filenames* — the missing piece is purely how a given `Image` widget picks *which* resolved resource
  it uses). `RenderImage` is the single highest-value next decompile target for unblocking #50 for real.
- **Follow-up decompile attempt (session 24 part 3, same sitting)**: targeted `RenderImage`,
  `CreateCachedRender`/`FindCachedRender`, `FindLibraryReference`, and `ReadLibraryRefsChunk`.
  `RenderImage__11cFEngRenderP7FEImageP14FERenderObjectP19FEPackageRenderInfo` (`80149198`) **failed to
  decompile** (`pcode error at 801491a0: Unable to resolve constructor` / "Bad instruction data" under
  `-noanalysis`). The other three decompiled cleanly and are useful, if tangential:
  - **`ReadLibraryRefsChunk__15FEPackageReader`** (`8013aa14`) confirms the on-disk `LibR` chunk
    (FourCC `0x5262694c` = `"LibR"`, matches the established reversed-FourCC convention) is a flat array
    of **12-byte records (3× u32 each, byteswapped on load)** — count = chunk length / 12. This is the
    format `.fng` files' library-reference sections use (currently only recorded generically as an
    `_other`/`_containers` entry by `fng_parse.py`, not yet parsed into structured fields).
  - **`FindLibraryReference__C9FEPackageUl`** (`8013a224`) does a **linear scan** over that same 12-byte
    record array (stored at `package+0x94`, count at `package+0x90`) comparing each record's first u32
    against a lookup key, returning the matching record's address (or 0) — i.e. `LibR` records are
    keyed by a hash/ID in their first u32, the same hash-lookup pattern already seen throughout this
    codebase (string hashes; sponsor/stage/message handles in the savefile RE). **This is a strong
    candidate mechanism for texture resolution**: an Image widget could carry a key elsewhere (not `If`)
    that resolves through this table to the actual texture resource — worth checking whether any
    currently-generic widget tag's value round-trips through a `LibR` record's first u32.
  - **`CreateCachedRender`/`FindCachedRender`** (`80149fa8`/`80149fa0`) confirm `FEObject+0x58` holds a
    **live cached-render-object pointer** (a doubly-linked-list node wrapping a `FERenderObject`,
    allocated on first render and cached thereafter) — a **separate field from `+0x5c`** (the `If`-tag
    texture-hash write target confirmed above), so no conflict with that finding; this is a render-cache
    mechanism, not the texture source itself.
- **`RenderImage` decompile retried with full Ghidra auto-analysis enabled (session 2026-08-30,
  continuing from session 24 part 3) — the earlier "needs full analysis" theory is now DISPROVEN.**
  Re-ran `analyzeHeadless` against the same `NFSU2.gpr`/`Speed.elf` **without** `-noanalysis` (full
  auto-analysis, ~31s, all standard analyzers ran clean, no errors) and re-targeted `RenderImage`
  (`80149198`) with the existing `DecompileFunc.java`. **Identical failure, same address**: `pcode error
  at 801491a0: Unable to resolve constructor`. Full analysis was not the missing ingredient.
  **Root-caused for real this time** by dumping the raw bytes at the failure point (new script
  `tools/ghidra_scripts/DumpRenderImage.java`, committed, reusable for any future "bad instruction"
  address by editing its `start` constant): the six 4-byte words at `801491a0`-`801491bb`
  (`f3 41 00 f0`, `f3 61 00 f8`, `f3 81 01 00`, `f3 a1 01 08`, `f3 c1 01 10`, `f3 e1 01 18`) all decode
  to **PowerPC primary opcode 60 (`0x3C`)**, which on the plain PowerPC ISA is unallocated/reserved but
  on the **Gekko/Broadway CPU (the actual GameCube/Wii-family core `Speed.elf`'s GC build targets) is
  `psq_st`** — paired-single quantized store, a Gekko-specific FPU extension (primary opcodes 4, 56, 57,
  60, 61 for `ps_*`/`psq_l`/`psq_lu`/`psq_st`/`psq_stu`) that plain IBM/Freescale PowerPC lacks.
  `RenderImage`'s prologue spills 6 paired-single register pairs to the stack (`f30`-something register
  numbers, offsets `0xf0`-`0x118` off `r1`) before doing any real work — consistent with a function that
  does 2D/vertex-style paired-float math (e.g. `{x,y}` or `{u,v}` pairs for the quad it draws), which
  fits "resolve and draw an Image widget" well. **Confirmed this Ghidra install has no Gekko/Broadway
  support at all**: `Ghidra/Processors/PowerPC/data/languages/*.slaspec` only has plain
  `ppc_32_be`/`_le`, `_4xx`, `_e500(mc)`, `_quicciii`, `ppc_64*` variants — no `gekko`/`broadway` SLEIGH
  spec, and no processor extension is installed under `Ghidra/Extensions/`. **This is the real,
  final root cause**: not a "needs deeper analysis" problem, but a genuinely missing instruction-set
  definition in the Ghidra install being used — no amount of re-running analysis on the same processor
  module will ever decode this function, because the decoder has no constructor for opcode 60 at all.
  **Concrete fix, not yet attempted**: install a community Gekko/Broadway Ghidra processor extension
  (the SLEIGH `psq_st`/`psq_l`/`ps_*` opcode definitions exist in open-source GC/Wii-decompilation
  tooling, e.g. the `ghidra-gekko-broadway-lang` extension used by the Dolphin-adjacent decomp
  community) into this `ghidra/Ghidra/Extensions/` directory, then re-open `Speed.elf` under that
  language (Ghidra requires an explicit language/processor reassignment for an already-imported
  program — not just re-running analysis) and retry the same `DecompileFunc.java` recipe. This is an
  environment/tooling change (installing third-party Ghidra extension code), not a docs or `.fng`-parser
  change, so flagging for explicit user sign-off before doing it rather than installing silently.
  **User approved (same session) — extension installed, RenderImage decompiled, and traced end-to-end.
  See the next section for the full findings.**
- **`"Oh"` is NOT a texture reference** (an earlier open question in this doc had left this
  unconfirmed). `ReadObjectTags` shows tag `0x684f` ("Oh") writes its 4-byte payload into the live
  `FEObject` at `+0x10` — **the same offset `OP`'s second field ("y", per `fng_parse.py`'s current
  labeling) writes to.** Real semantics of this shared field are still unclear (candidates: object/
  parent-linkage hash, not a screen Y-coordinate at all — the field being writable by two different tags
  makes a coordinate interpretation less likely), but the earlier working guess that `OP`'s 4 fields are
  cleanly `{x, y, flags, extra}` is now **directly contradicted** for the second field. `fng_parse.py`
  updated this session to capture `Oh`'s value separately (`objHash`) rather than silently dropping it
  into the generic tag bag, but the `x`/`y` labels on `OP`'s fields should be treated as unreliable until
  this is resolved further.
- Confirmed tag map (2-byte tag -> target, from the `ReadObjectTags` dispatch): `Ot`=create object
  (`CreateObject__15FEPackageReaderUl`, existing knowledge), `OP`=4-field generic record at object
  `+0xc`/`+0x10`/`+0x1c`(flags, `&0x100000`/`&0x10000000` bits checked)/`+0x22`(u16), `On`=object name
  (`SetName__8FEObjectPCc`), `Oh`=see above, `PA`=parent-object lookup by GUID
  (`FindObjectByGUID__9FEPackage`Ul`), `SA`=a variable-length float/word array copied into the object at
  a pointer read from `+0x2c` (script/animation keyframe data, matches the long hex blobs seen in real
  `SA` tag dumps). All other tags fall through to the per-type processor (`ProcessImageTag` for types
  1/9/10, `ProcessStringTag` for type 2, `ProcessCodeListBoxTag` for types 6-10 excluding 9, `ProcessListBoxTag`
  for type 4).
- `ProcessStringTag` tag map: `Sj`(`0x6a53`)=`+0x6c` raw u32, `SL`(`0x4c53`)=literal label
  (`SetLabel__8FEStringPCc`), `SH`(`0x4853`)=`+0x60` string-hash u32 (existing knowledge, now confirmed
  at the live-object level too — also triggers a `0x400000` dirty-flag bit and a callback), `Sb`
  (`0x6253`)=`SetLength__12FEWideStringUl` at `+100`, `St`(`0x7453`)=wide-string assign + byteswap loop
  (`+100`), `Sl`(`0x6c53`)=`+0x70` raw u32, `Sw`(`0x7753`)=`+0x74` raw u32.
- `ProcessListBoxTag` handles the `CodeListBox` widget-type's per-cell tags (`Cr`/`Lc`/`Ci`/`Ct`/`Cc`,
  etc.) — cell coordinates, cell type, cell string/color per row/column, building a per-cell data table
  via `GetPCellData__9FEListBoxUlUl`. Not relevant to `UI_Main.fng` (a `String`/`Image`/`Group`-only
  screen per earlier RE), so not prioritized for further decode — documented here for completeness only.

## `RenderImage` decompiled for real (session 2026-08-30, later session) — texture-resolution chain traced

Installed `aldelaro5/ghidra-gekko-broadway-lang` (5 files: `.cspec`/`.ldefs`/`.pspec`/`.slaspec`/`.sinc`,
downloaded via `curl` from the repo's `master` branch — no build step needed, Ghidra compiles the `.sla`
from `.slaspec` automatically on first use) into `ghidra/Ghidra/Processors/PowerPC/data/languages/`. The
new language id `PowerPC:BE:32:Gekko_Broadway:default` shows up automatically (its own `.ldefs` is
picked up without touching the base `ppc.ldefs`). **Note for reproducing:** reassigning an *existing*
imported project file to a new language isn't a simple flag on `-process`; had to `-import` the raw
`Speed.elf` fresh (copied to a space-free path first — `analyzeHeadless.bat` breaks when both the
project args and an `-import` path contain spaces, a Windows batch-quoting issue, not a Ghidra one)
with `-processor "PowerPC:BE:32:Gekko_Broadway" -cspec default`, landing as a second program
`Speed_gekko_import.elf` in the same `NFSU2` project alongside the original `Speed.elf` (kept, not
overwritten — every prior session's decompile target still works fine under the old language for any
function that doesn't touch paired-single opcodes). **Use `Speed_gekko_import.elf` as the `-process`
target for any future Image/render-path decompile** — the original `Speed.elf` stays fine for
everything else.

`RenderImage__11cFEngRenderP7FEImageP14FERenderObjectP19FEPackageRenderInfo` (`80149198`) now decompiles
cleanly (confirms the earlier byte-level diagnosis exactly — the prologue's six `psq_st`/`psq_l` pairs
are just non-volatile paired-single register spill/restore around the function body; Ghidra's
decompiler folds them into ordinary `double`-typed locals once the language actually understands them).
**The real finding:**

```c
iVar2 = GetTextureInfo__FUiii(*(undefined4 *)(param_2 + 0x24), 1, 0);
...
*(int *)(param_3 + 0x14) = iVar2;   // resolved TextureInfo* stashed on the FERenderObject
```

**The texture hash actually consumed at render time lives at `FEObject+0x24`, NOT `+0x5c`** (the field
`If` writes to). `+0x5c` and `+0x24` are two different, unrelated fields — `If`/`+0x5c` is written but
apparently never read by the render path at all (at least not by `RenderImage`); it may be dead/vestigial
in the shipped game, or consumed by some other subsystem not yet found. Decompiled `GetTextureInfo__FUiii`
(`800cfe54`) to confirm what `+0x24` actually is: it's a **hash key into a global texture-pack registry**
— walks a `TexturePack` linked list, checks `TextureLoadedTable` (a `LoadedTable` keyed by the hash) via
`GetLoadedTexture__11TexturePackUi`/`GetTexture__11TexturePackUi`, falling back to
`eGetRenderTargetTextureInfo__Fi`/`eGetOtherEcstacyTexture__FUi` for render-target/special textures if
not found in a pack. This confirms textures are resolved through a **runtime-global texture-pack system
keyed by hash**, not through the `.fng` file's own local `Res\xCC`/`RsNm` resource table at all — that
table (session 24's `(a)` work) resolves resource *names*, but the actual GPU-facing lookup goes through
a completely separate hash-keyed pack/pool system. This is a materially different picture from the
original assumption that `Res\xCC`/`RsNm` would be the direct source of a widget's texture.

**So where does `FEObject+0x24` actually get its value, if not from `If`?** Decompiled the full
`ReadObjectTags__15FEPackageReaderP5FETagUl` (`8013b584`, previously only partially known) and
`CreateObject__15FEPackageReaderUl` (`8013b2e0`) to find out:

- **`CreateObject`** (called for the `Ot` tag) explicitly zeroes `+0x5c` for every widget type it
  constructs (Group/String/CodeListBox/Image/Movie/AnimImage/SimpleImage/ColoredImage) but **never
  touches `+0x24` or `+0x28` for any type, ever** — those bytes are whatever the underlying allocator
  (`FEngMalloc__FUiPCci`) handed back, uninitialized as far as this function is concerned.
- **`ReadObjectTags`'s full tag dispatch** (now completely known: `Ot`=0x744f create, `OP`=0x504f the
  4-field record, `On`=0x6e4f name, `Oh`=0x684f the shared-with-OP's-"y" field `+0x10` write,
  `PA`=0x4150 parent-lookup, `SA`=0x4153 script/anim array, everything else falls through to the
  per-type processor) has **exactly one place `+0x24`/`+0x28` are ever written**, and it's inside the
  `OP` tag's handler, conditionally: when the just-parsed `+0x1c` flags word has bit `0x100000` set
  (a "this object is a reference to another object" flag), the code calls
  `FindReferencedObject__15FEPackageReaderUlPP8FEObjectPP9FEPackage` (`8013d134`, also now decompiled)
  to resolve a **cross-package template object** — it looks up the local object's GUID (`+0xc`, the
  `OP` tag's first field) in the `LibR` hash table (`FindLibraryReference__C9FEPackageUl`, already known
  from session 24 part 3), uses that record's package-id field to find the actual already-loaded
  library package (`FindLibraryPackage__C7FEngineUl` on the global `FEngine`), then resolves the
  referenced object inside *that* package by GUID (`FindObjectByGUID__9FEPackageUl`). **If the local
  object's own `+0x22` field (a u16 written by `OP`'s 4th field, previously only guessed to be
  "flags-adjacent") equals the sentinel `-1` (0xFFFF), the code copies `+0x24` and `+0x28` straight from
  the resolved referenced object into the local one** (the found template object's own `+0x24`/`+0x28`
  get copied into the object currently being built).
- **Conclusion: Image widgets in ordinary screen `.fng` files (like `UI_Main.fng`) don't carry their own
  texture hash at all — they're references (via `OP`'s GUID + the `0x100000` "is a reference" flag) to a
  widget object that lives inside a separately-loaded *library* package** (loaded via the `LibL`/`LibR`
  mechanism decompiled in session 24, e.g. the `GenericDialog_*.fng`-style shared-template files), and
  it's *that* library object's `+0x24`/`+0x28` that get inherited. This directly validates and sharpens
  session 24 part 3's "`LibR`/`FindLibraryReference` is a strong candidate" guess — it wasn't just
  "worth checking," it's the literal, exact mechanism, now traced to working decompiled code rather than
  inferred from record shape alone.
- **Root-template `+0x24` origin — SOLVED (same session, continued).** Decompiled
  `Startup__9FEPackageP15FEGameInterface` (`80138fd4`): after calling the game-interface's init callback,
  it calls `ConnectObjectResources__9FEPackage` (`8013a0fc`) — a one-liner that walks every object in the
  just-loaded package via `ForAllObjects__9FEPackageR16FEObjectCallback`, dispatching each one through a
  `ResourceConnector` callback object's vtable (`_vt_17ResourceConnector`). Dumped that vtable and
  decompiled its real callback, `Callback__17ResourceConnectorP8FEObject` (`80139f3c`):
  ```c
  undefined4 Callback__17ResourceConnectorP8FEObject(int param_1,int param_2)
  {
    int iVar1 = *(int *)(param_2 + 0x18);          // object's widget-type code
    if (iVar1 == 4) {
      ConnectListBoxResources__17ResourceConnectorP9FEListBox();   // per-cell resources, ListBox only
    }
    else if (((iVar1 < 4) || (6 < iVar1)) && (*(ushort *)(param_2 + 0x22) != 0xffff)) {
      iVar1 = (uint)*(ushort *)(param_2 + 0x22) * 0x18;
      *(undefined4 *)(param_2 + 0x28) = *(undefined4 *)(iVar1 + **(int **)(param_1 + 8) + 0x14);
      *(undefined4 *)(param_2 + 0x24) = *(undefined4 *)(iVar1 + **(int **)(param_1 + 8) + 0x10);
    }
    return 1;
  }
  ```
  **This is the missing mechanism, and it resolves several previously-mysterious fields at once:**
  - `FEObject+0x22` (the `OP` tag's 4th field, previously only known as "possibly flags-adjacent, or the
    reference sentinel when `-1`") **is a u16 index into the package's own `Res\xCC`/`RsRq` resource-record
    table**, not a flags field. `0xFFFF` isn't just "the reference sentinel" in the abstract — it's
    literally "this object has no direct resource-table entry," which is exactly the condition
    `ReadObjectTags`'s `OP`-tag handler already checked before falling back to the cross-package
    `FindReferencedObject` copy (see previous bullet). The two mechanisms are two branches of the *same*
    design: an object either carries its own resource index (resolved here, at post-load `Startup` time,
    against its own package's local `Res\xCC`/`RsRq` table) or it's a reference to another package's
    template object (resolved earlier, at parse time inside `ReadObjectTags`, by copying that template's
    already-known `+0x24`/`+0x28`).
  - The `RsRq` 24-byte record's previously-unlabeled tail fields are now identified: **`field4` (byte
    offset `+0x10` in the record, `{field0, nameOffset, field2, field3, field4, field5}` per session 24's
    layout) is the texture hash copied into `FEObject+0x24`**, and **`field5` (`+0x14`) is a secondary
    value copied into `FEObject+0x28`** (likely a size/pack-id/flags word paired with the hash — not yet
    individually pinned down, but no longer relevant to unblocking #50 since `+0x24` alone is what
    `GetTextureInfo`/`RenderImage` consume).
  - Widget types excluded from direct resource-connect here: type `4` (ListBox, handled by the separate
    per-cell `ConnectListBoxResources` path) and type `5`/`6` (`iVar1 < 4 || 6 < iVar1` excludes both —
    `5` is `Group`, which has no texture, matching `CreateObject`'s record that Group's own `+0x5c` write
    is its only resource-adjacent field and it has no `+0x24` slot semantics at all; `6` is
    `CodeListBox`, presumably resolved through its own per-cell path like `4` rather than shown here).
  - Confirmed this is genuinely a *post-load, whole-graph* pass, not a per-tag one: `ConnectObjectResources`
    runs once, after every chunk (including `ObjD`) has been fully parsed for the package, which is why
    it can't be found by looking at `ReadObjectTags`/`ProcessImageTag` alone — it runs on a separate later
    pass over already-fully-built objects.
  - **#50's texture-assignment mechanism for `.fng` Image widgets is now fully traced end-to-end, with no
    remaining unknowns on the critical path**: `OP` tag's `+0x22` field → either a local
    `Res\xCC`/`RsRq` table index (resolved post-load by `ResourceConnector::Callback`) or the `-1`
    reference sentinel (resolved at parse time via `FindReferencedObject`'s cross-package template copy)
    → `FEObject+0x24` texture hash → `GetTextureInfo__FUiii` (runtime `TexturePack`/`TextureLoadedTable`
    lookup, with render-target/Ecstacy-texture fallbacks) → `TextureInfo*` consumed by `RenderImage`.
    `+0x5c`/`If` remains confirmed dead/unused by this entire pipeline. The only field left un-individually-
    pinned is `RsRq` record's `field0`/`field2`/`field3` and the exact meaning of `+0x28`'s `field5` — none
    of which block rendering, since `+0x24` alone is sufficient to resolve a texture.

## Session 2026-08-30 — position/size finally traced: SA tag, not OP, and it's not a coordinate pair at all

`#50`'s other original blocker (besides the hash-space mismatch above) was that `OP`'s first two 4-byte
fields looked like they could be an x/y position pair, but rendering with them as literal coordinates
produced nonsense. This was already partly explained above (`+0xc` is a cross-package GUID, `+0x10` is
shared with `Oh` and is "not a screen Y-coordinate at all") but where the *real* position data lives was
still unknown. Surveyed `Gamecube Files/Speed.elf` via `nm` for track/position-related symbol names, then
Ghidra-decompiled the real hits:

- `SetPosition__8FEObjectRC9FEVector3b` (`801385d0`): reads a `Vector3` from the object's per-instance
  "track data" buffer at offset `+0x1c` from that buffer's base, then calls
  `SetTrackValue__8FEObject18FEKeyTrack_IndicesRC9FEVector3b(obj, /*index*/ 2, ...)`.
- `SetSize__8FEObjectRC9FEVector3b` (`80138858`): same pattern, buffer offset `+0x38`, track index `4`.
- `GetDataOffset__8FEObject18FEKeyTrack_Indices` (`80138b70`) is the index -> byte-offset lookup table for
  that track buffer (`FEObject+0x2c` holds the buffer pointer): `0->0x0, 1->0x10, 2->0x1c, 3->0x28,
  4->0x38, 5->0x44, 6->0x4c, 7->0x54, 8->0x64, 9->0x74, 10->0x84`. Confirms track 2 = Position (`0x1c`),
  track 4 = Size (`0x38`); track 3 (`0x28`, 16 bytes, exactly filling the gap between Position's end and
  Size's start) is inferred to be Rotation (`FEQuaternion`, 4 floats) though no direct `SetRotation` caller
  was found to confirm the index number itself.
- `ReadObjectTags`'s `SA`-tag handler (tag code `0x4153`, byteswap is GC-only and doesn't apply on
  PC/3DS's shared little-endian) copies the `SA` chunk's payload **word-for-word into that same track
  buffer, starting at buffer offset 0** — i.e. SA payload byte N == track buffer byte N. So the position/
  size/rotation floats are readable directly out of the raw `SA` tag bytes at the offsets above, with no
  runtime computation needed for a static rest pose.

**Verified against real bytes**, not just decompile theory: a scratch script walked every `ObjD` in the
real `extracted/fng/UI_Main.fng` and decoded each widget's `SA` payload at these offsets. All 55 widgets
carry an `SA` tag. Offset `0x28` (rotation) decodes to the identity quaternion `(0, 0, 0, 1)` for every
single widget. Offset `0x38` (size) decodes to sane on-screen pixel dimensions for every widget (`385x32`,
`118x59`, `32x32`, `167.35x32`, etc. — matching real UI element proportions). This combination (a
constant, meaningful identity value for the field guessed as rotation, and plausible pixel sizes for the
field guessed as size, across all 55 independent widgets) is not something that happens by accident from
a wrong offset guess — it's confirmation.

Position (offset `0x1c`, 3 floats: x, depth, z) reads back as values roughly in the ±300–400 range on both
the x and z axes, consistent with an engine coordinate space centered at the screen/canvas's own origin
(not top-left-anchored like typical 2D UI coordinates) — `z` is the on-screen vertical axis, `y` (the
middle float) appears to be a depth/layer value, unused for 2D placement. The exact reference resolution
this space was authored against (640x480? 800x600?) is not yet pinned down; `3DS Files/source/main.c`
currently maps it onto the 3DS top screen with a first-pass, uncalibrated `screenX = 200 + posX,
screenY = 120 - posZ`, pending a clean screenshot to calibrate against.

`OP`'s role is now fully separated from position/size: it only ever carries the GUID/reference-lookup and
resource-index fields described above. Position, rotation, and size for every widget come from `SA`
exclusively.

## Open questions / next concrete steps

1. Decode `PC Files/LANGUAGES/English.bin`'s string-table format (header at
   offset 0 has two large `u32` offsets into the 270KB file, structure not
   yet decoded) so `SH` language-hash tags in `.fng` widgets (confirmed
   format above) can be resolved to real display text -- this directly
   blocks getting real legal-screen/menu text on screen instead of just
   structural widget data.
2. `tools/fng_parse.py` now exists and works (see above) -- `GenericDialog_
   SMALL.fng`/`LS_Demo_Legal.fng`/`UI_Main.fng` all parse cleanly to
   structured JSON. Remaining parser gaps: `PA`/`St`/etc. tag payloads are
   still stored as raw hex rather than semantically decoded. `OP`'s x/y
   fields are now known to NOT be position data (see the SA-tag session
   above) -- resolved, no longer an open question. `tools/fng_parse.py`
   itself still needs updating to decode `SA` as position (`+0x1c`)/
   rotation (`+0x28`)/size (`+0x38`) the way `3DS Files/source/fng.c` now
   does, and to use the real `GetBaseName`/`ToUpper`/`bStringHash` texture-
   hash resolution instead of the raw on-disk `RsRq.hash` field -- both
   fixes currently only live in the C port, not the Python reference parser.
3. Decompile the remaining chunk-content readers -- `ReadResourceChunk`
   (0x8013ab50, chunk tag `Res\xCC`/`RsNm` confirmed), `ReadPackageResponseChunk`
   (0x8013ae54, tag `PkgR` confirmed), `ReadMessageTargetListChunk`
   (0x8013cf34, tag `Targ` confirmed), plus `ReadReferencedPackagesChunk`
   (0x8013a858) and `ReadLibraryRefsChunk` (0x8013aa14, tags not yet seen in a
   real file) -- rounds out the full container spec so the parser can read
   every chunk type, not just objects/TypeSizes/Resources.
4. Cross-reference against the PC `SPEED2.EXE` build once the GC-side pattern
   is understood, since the PC build is the actual target being ported (GC is
   a naming/structure reference only, per `HANDOFF.md`).
5. `BootFlowManager`'s screen list (the actual boot sequence order: EA logo ->
   ESRB -> legal -> language select -> ... -> `UI_Main.fng`) is still not
   dumped -- decompiling its constructor/init would give the exact real boot
   sequence to reproduce. (Session 14 implemented the EA-logo-audio portion
   of this on the 3DS side using the real extracted `ealogo.vp6` audio track
   -- see `HANDOFF.md` session 14 -- but the legal/menu screens after it are
   still blocked on item 1 above, real text resolution.)

This is multi-session RE work by nature (80+ screens, an object/widget
framework, plus a still-unparsed `ObjD` widget record format) -- do not
attempt to shortcut it by inventing a placeholder menu; see project scope
note in `HANDOFF.md`.

## `IconScrollerMenu` activate guard and the menu-item flag byte (session 2026-09-10, task #118)

`FUN_00543d40` is the `IconScrollerMenu` base message handler. Its `0x0C407210` (activate / `A`)
branch is the only per-item "you cannot pick this" gate in the frontend, and it works off two
bytes on the *selected item* (`this+0x58`), not off any list-node payload:

| Offset on the menu item | Meaning |
| --- | --- |
| `+0x20` | hard disable; blocks activation, no sound swap of its own |
| `+0x21` | set to 1 by the base ctor `FUN_0051f670` (visible/enabled) |
| `+0x22` | "nothing behind this item" -- blocks activation **and** swaps the sound |
| `+0x23` | 0 in the base ctor |
| `+0x44` | 1 in the base ctor |
| `+0x45` | 0 in the base ctor, **1** in the `FUN_00520cb0` subclass = "has its own activate handler" |

The branch, condensed:

```c
cVar1 = *(char *)(param_1[0x16] + 0x22);          // this+0x58 -> selected item
if (cVar1 != ' ') { local_8 = 0x13; }            // denied sound
if (list_non_empty && *(char *)((int)param_1 + 0x7b) != ' '   // input enabled
    && *(char *)(param_1[0x16] + 0x20) == ' ' && cVar1 == ' ') {
  if (param_1[0x16] == 0 || *(char *)(param_1[0x16] + 0x45) == ' ')
       { FUN_0051fd10(); FUN_0052fbf0(); }        // default activate
  else (**(code **)(param_1[0x13] + 0xc))(0xc407210, ...);      // item's own handler
  local_8 = 8;                                    // accept sound
  FUN_0050da20(0xc407210, ...);                   // forward to the screen
}
LAB_00543f99:
  iVar3 = (int)*(char *)((int)&DAT_00837fe0 + iVar3 + local_8 * 0x1b);
  if (iVar3 != -1) FUN_00458520(iVar3);
```

Sounds come from the table at `DAT_00837fe0`, **stride `0x1b`**, indexed by the event slot
(`8` = accept, `0x13` = denied) plus a per-screen base; `-1` means silent. `FUN_00458520` plays it.

`+0x22` is written to 1 in exactly three places -- `FUN_00545700` (`0x5457ff`), `FUN_00545920`
(`0x545ace`) and `FUN_005463b0` -- and in all three it is `(submenu item count == 0)`. So the
flag means *empty submenu*, and there is no career/unlock feeding into it anywhere. This is why
retail does **not** refuse `A` on a padlocked track row (see task #118): the lock byte
`node+8`, written by `FUN_004b4c10` as `1 - IsUnlocked(record+0x8A)`, is read only by the sort
comparator `FUN_004987a0` and by `FUN_004cded0`'s cursor restore.

Related structures decoded alongside:

* `FUN_0052ec90` -- scroller element factory. 0x2c-byte element, vtable `&PTR_FUN_0079afdc`,
  writes `+0x20 = param_3`, `+0x21 = param_4`, `+0x22 = 0`, `+0x23 = 0`, links `elem+4` into the
  owner's list at `owner+4`/`+8`, and returns the widget at `elem+0x10`.
* `FUN_0050c990` -- image-hash setter: `if (obj && obj[0x24] != hash) { obj[0x24] = hash;
  obj[0x1c] |= 0x400000; }` (the `0x400000` bit is the redraw-dirty flag).
* Track-list node (`FUN_004b4c10`), 0x30 bytes: `+0 next`, `+4 prev`, `+8 locked`, `+0x0c = 0`,
  `+0x10 = 0`, `+0x2c = record`. Sentinel pair at `screen+0x4c`/`+0x50`, count at `screen+0x150`.
  Note `FUN_00543d40`'s empty test compares `*(this+0x50)` with `this+0x50`, which is off by four
  from that convention -- unresolved, either a retail off-by-4 or per-derived-screen slot reuse.

## `EA_TRAX_Jukebox.fng`: the row builder (session 2026-09-10, task #127)

Two functions own this screen.

`FUN_00553C70` is the ctor. It sets the scroll area up with
`FUN_00542CA0(pkg, "Scroll Area", "ScrollBar", 1,0,0,1)`, presets `this[0x49]` (= `this+0x124`) to
**4** -- the number of visible rows, and therefore the page step -- and binds:

| hash | type | slot |
| --- | --- | --- |
| `0xCA74A2FA` | String | `this[0x41]` (play-order label) |
| `0x444969FD` / `0x444969FE` | Image | `this[0x3E]` / `this[0x3F]` (scroll arrows) |
| `0xCF6F0439` / `0xABA93D3F` | Image | `this[0x43]` / `this[0x44]` |
| `0x8065767B + i` | Group | `this + 0x45 + i` (per-row state group; the ctor then selects the child whose `+0x30` is `hash("static")`) |

It picks the play-order label's string hash out of `DAT_0083AA58` (`0 -> 0x4CA36B89` =
"Sequential", `1 -> 0xA9C9C8F7` = "Random", anything else 0), writes it to `label+0x60` with the
`0x400000` dirty bit, and tail-calls the populate pass.

`FUN_0054B310` is that pass, and it runs two loops.

1. `this+0x124` (4) iterations building 0x3C-byte row nodes (vtable `&PTR_FUN_0079B9D4`,
   `Highlight` / `UnHighlight` at node[0xB]/[0xC], linked at `this+0x54`/`+0x58` with the count at
   `this+0x64`). Each node takes `"Left arrow %d"` / `"Right arrow %d"` (formatted with `i+1`) at
   node[0xD]/[0xE] -- these resolve to `0x7BCD6704 + i` / `0x7C51B6D7 + i`, the *same* two base
   hashes `UI_QRModeOptions` uses for its adjust arrows -- the group `0xDE635F7C + i` at node[5],
   and the five text cells. Row geometry: `node[6] = -283.0f`, `node[7] = (float)(i*0x50 - 0x87)`
   (pitch **80**), `node[8] = 540.0f`, `node[9] = 70.0f`.
2. **27** iterations (`while (iVar15 < 0x1b)`) building 0x20-byte track nodes (vtable
   `&PTR_FUN_00795CB0`, linked at `this+0x5c`/`+0x60`, count `this+0x68`). The row number is
   formatted with `&DAT_0079C730` (`"0%d"`) when `i < 9` and `&DAT_007845D4` (`"%d"`) from ten up;
   artist, title and third line are appended via `FUN_004AEA60` out of the 16-byte records based at
   VA **0x7ECD80** (`+0` title, `+4` artist, `+8` album), indexed by `(&DAT_0083ACB8)[i*2]`, with
   that index stored at `node[6]` and `(&DAT_0083ACBC)[i*8]` at `node+0x1C`; the duration string
   comes from `FUN_004FF9D0()` + `FUN_0043DC00(buf, i, 0x10)`. The tail dirties the scroll arrows
   at `this+0xF8`/`+0xFC` (`0x400001` vs `0x400000`) by whether the selection is at the head or the
   tail of the list.

The per-row widget hashes are row 1's plus the row index, which is why a search of the exe for them
only ever hits row 1:

| cell | base hash |
| --- | --- |
| number | `0xE454E9A5` |
| title | `0x2890C8AF` |
| artist | `0x66646FC4` |
| album / third line | `0x77BD189E` |
| enable value | `0xF3EBDC4E` |

Worth knowing about the authored data: **none of these cells carries an `SH`**. They have only an
`St` authoring placeholder (row 1 reads `01` / `Swimming with the Sharks` / `Eddie & The Springbok
Nude Girls` / `Need for Speed: Underground 2 Exclusive`) and a `Sb` buffer width of 200 for the
three text lines. The enable cell's placeholder is `ON`, which is its English.bin *key*, not its
display text -- `b_string_hash("ON")` is `0x63C` and the table maps it to "On" (`0xCCFA` -> "Off").
Every cell is authored at colour (50,50,50); the engine's per-row Highlight / UnHighlight objects
are what make a row readable, so a port without those has to supply its own tint.

## Ghidra headless decompile recipe (reusable)

```bash
# 1. Write/update a GhidraScript at <scratch>/DecompileFunc.java with the
#    target addresses (see git history of this repo's scratch dir, or just
#    recreate: DecompInterface.openProgram(currentProgram), then for each
#    address, currentProgram.getFunctionManager().getFunctionAt(addr) and
#    decompiler.decompileFunction(f, timeoutSeconds, monitor).getDecompiledFunction().getC()).
# 2. Run against the existing project (already has SPEED2.EXE and Speed.elf
#    imported at path "/", so no re-import needed):
cd "<repo's parent Development dir>"
./ghidra/support/analyzeHeadless.bat "GhidraProjects" "NFSU2" \
    -process "Speed.elf" -noanalysis \
    -scriptPath "<scratch dir containing DecompileFunc.java>" \
    -postScript DecompileFunc.java
# 3. Read whatever output file the script wrote.
```
`-process "Speed.elf"` selects which of the two imported programs to target;
swap in `"SPEED2.EXE"` to decompile the PC build instead (note: PC build is
stripped, so use symbol addresses cross-referenced from the GC build's
already-known function purposes, not names, when working on it).
