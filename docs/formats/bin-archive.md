# NFSU2 `.BIN` / Chunk resource format

Status: **reverse-engineered from both builds now**. GameCube (`Speed.elf`, unstripped) gave the
original class/function names and the low-level `bChunk` header/dispatch mechanics. PC
(`SPEED2.EXE`) is fully stripped of custom symbol names, but a handful of debug/log strings
(`ResourceFileSlotPool`, `RegionLoader::LoadHandler`, `TrackLoader::LoadHandler`) survived in
`.rdata` and gave direct xrefs into the equivalent PC functions — see "PC cross-reference" section
below. This is EA Black Box's generic "Chunk" resource container, used across multiple
Blackbox-engine titles, not something NFSU2-specific — the class names (`bChunk`, `ResourceFile`)
live in shared engine code ("bWare").

## Source evidence

All of the below comes from decompiling these GameCube functions (Ghidra, PowerPC:BE:32:default,
symbols from `Speed.elf`'s CodeWarrior debug symtab):

- `EndianSwapChunkHeader__FP6bChunk` @ `0x80194890`
- `EndianSwapChunkHeadersRecursive__FP6bChunki` / `...T0` @ `0x801948dc`
- `CallChunkLoader__FP6bChunk` / `CallChunkUnloader__FP6bChunk` @ `0x8019438c` / `0x80194424`
- `FindLoader__12bChunkLoaderUi` @ `0x80058e5c`
- `IsTempChunk__FP6bChunk` @ `0x80194988`
- `__12ResourceFilePCc16ResourceFileTypeiii` (ResourceFile ctor) @ `0x80194d2c`
- `CreateResourceFile__FPCc16ResourceFileTypeiii` / `LoadResourceFile__...` @ `0x80195204` / `0x80195278`

## Chunk header (8 bytes, confirmed)

```c
struct bChunkHeader {
    uint32_t type;   // offset 0x0 — chunk type ID, dispatch key for loader/unloader tables
    uint32_t size;   // offset 0x4 — payload size in bytes (extent for recursion, see below)
};
```

Confirmed directly: `EndianSwapChunkHeader` swaps exactly two consecutive `uint32`s at offsets 0
and 4 of the pointer passed in — nothing else. This is the whole header. GameCube data is
big-endian; PC data is presumably little-endian raw (no swap needed there), so on PC this function
either doesn't exist or is a no-op — worth confirming when cross-referencing.

## Nesting (hierarchical container, confirmed)

`EndianSwapChunkHeadersRecursive(chunk, size)` calls the two-pointer-argument variant with
`(chunk, chunk + size)` — i.e. it walks from `chunk` up to `chunk + size` swapping headers as it
goes, recursing into sub-chunks. This confirms chunks nest: a chunk's payload can itself contain a
sequence of child `bChunkHeader`-prefixed chunks, extent-bounded by the parent's `size` field. This
is the classic RIFF/IFF-style nested-chunk container shape.

## Dispatch (type → handler, confirmed)

- `bChunkLoaderFunctionNull(chunk)` just checks `chunk->type == 0` — a sentinel/terminator loader.
- `FindLoader(type)` hashes `type` (`(type + type>>6 + type>>12) & 0xfc`, i.e. a 64-entry
  open-hash table keyed off a mix of the type ID's bit ranges) into `_12bChunkLoader_sLoaderTable`,
  a `bChunkLoader`-node linked list (12-byte objects — hence the mangled `12bChunkLoader`).
  Each node's layout (inferred from access `piVar1[1] == type` and calling
  `(**(code**)(node+8))(chunk)`): `{ next*, type, loaderFnPtr, unloaderFnPtr }`-ish, i.e. a
  registered `(type → loader fn, unloader fn)` mapping.
- `CallChunkLoader(chunk)`: looks up `chunk->type` in that hash table; if found, calls the
  registered loader fn pointer at node-offset `+8`; if not found, falls back to walking a global
  `LoaderTable` array of fallback loader functions until one returns nonzero (handled) or the table
  is exhausted. `CallChunkUnloader` mirrors this with `UnloaderTable` / node-offset `+0xc`.
- This means every subsystem in the engine (car parts, textures, lights, sound, anim, camera sets,
  FE menus, movies, career data, etc.) registers its own `LoaderXxx__FP6bChunk` /
  `UnloaderXxx__FP6bChunk` pair keyed by a unique chunk-type constant, and the resource file's
  chunk stream is walked generically by chunk type, dispatching into whichever subsystem owns that
  type. ~120+ such Loader/Unloader pairs exist in `Speed.elf` alone (see `gc_symbols.txt` — every
  `*__FP6bChunk` function name is a registered handler for one subsystem's chunk type).

## `ResourceFile` (the archive/file wrapper, partially mapped)

Constructor `ResourceFile(this, char* path, ResourceFileType type, int a, int b, int c)` sets, on
the object (offsets relative to `this`):

| offset | field (inferred) |
|--------|-------------------|
| 0x08   | constant `1` (some state/refcount init) |
| 0x0c   | `type` param (the `ResourceFileType` enum value) |
| 0x10, 0x14, 0x18 | the three trailing int params (purpose TBD — one is used later: if nonzero, overrides computed file size at 0xdc) |
| 0x1c   | file path string (via `bStrCpy`, so this is a fixed-size inline char buffer starting here, not a pointer) |
| 0xd8   | zeroed (flag?) |
| 0xdc   | file size in bytes, from `bFileSize(path)`, clamped to ≥0, optionally overridden by the `c` param at 0x18 |
| 0xc8, 0xcc, 0xd0, 0xd4, 0xe0, 0xe4, 0xe8, 0xec | zeroed at construction (state fields, purpose TBD) |
| 0x58   | zeroed byte flag |

`CreateResourceFile` heap-allocates via `bOMalloc` from a slot pool and links the new object into a
global doubly-linked list (`gAnimLoader_ResourceFileList`-style pattern — an intrusive linked list
node lives at offsets 0x0/0x4 of the object, i.e. *before* the fields above, or the ctor's
`param_1` already points past a list-node header — needs one more decompile pass to confirm exactly
where the ctor's `this` starts relative to the allocated block).

`LoadResourceFile(path, type, ...)` = `CreateResourceFile(...)` then `BeginLoading(...)` — i.e.
opening a resource file is a two-step construct-then-kick-off-async-load, consistent with this
being a streaming/async resource system (see also `eStreamPackLoader` class spotted in the symbol
list, a related but distinct streaming-pack system — not yet investigated).

## Confirmed against a real PC `.BIN` file (`CARS/240SX/GEOMETRY.BIN`, first 0x50 bytes)

```
0000: 0040 1380 5024 7300 0000 0000 0000 0000
0010: 0140 1380 d04c 0000 0240 1300 9000 0000
0020: 0000 0000 0000 0000 1d00 0000 6102 0000
0030: 2e2e 5c50 435c 4344 5547 325c 4341 5253   ..\PC\CDUG2\CARS
0040: 5c32 3430 5358 5c47 454f 4d45 5452 592e   \240SX\GEOMETRY.
0050: 4249 4e00 ...                             BIN.
```

Reading as little-endian `{type: u32, size: u32}` pairs exactly matches the GameCube-derived
8-byte header theory, **and** reveals one more convention: the top bit of `type` (`0x80000000`)
marks a *container* chunk (payload is itself a sequence of child chunks) vs. a *leaf* chunk
(payload is raw data):

- offset `0x00`: `type=0x80134000` (container, top bit set), `size=0x00732450` (7,548,496 bytes) —
  top-level container for the whole geometry chunk tree.
- offset `0x10`: `type=0x80134001` (container), `size=0x00004cd0` (19,664 bytes) — first child of
  the above, itself a container.
- offset `0x18`: `type=0x00134002` (leaf, top bit clear), `size=0x00000090` (144 bytes) — first
  child of *that*, a leaf. Its payload (starting `0x20`) is a small record (`0x1d`, `0x261`, ...)
  followed by an embedded debug source path string
  (`..\PC\CDUG2\CARS\240SX\GEOMETRY.BIN`) — almost certainly an asset-pipeline debug/provenance
  leaf chunk, not gameplay data.

No padding between a chunk's 8-byte header and its payload, and no padding between a container's
last-consumed byte and the next sibling header — chunks pack tightly, extent-bounded purely by
`size`. This is a live, independent confirmation of the GameCube decompile's header theory using
actual PC asset data, which is a good sign the same chunk-type IDs are shared verbatim between the
PC and GameCube builds (`0x80134000`/`0x80134001`/`0x00134002` are presumably geometry-subsystem
chunk types registered on both platforms).

## PC cross-reference (session 3)

`SPEED2.EXE` has zero surviving symbol names for anything game-specific (confirmed: grepping its
40k-entry symbol table for `Chunk`/`Resource`/`Loader` only turns up Win32 API imports like
`LoadResource`/`FindResourceA`, which are unrelated — those are the standard Windows PE resource
functions, not the game's `ResourceFile`). However, several **debug/log strings** in `.rdata`
survived and gave a direct path in via Ghidra's reference-to-string search (`FindStringXrefs.java`
headless script, searches all defined string data for a needle then decompiles every xref'ing
function):

- `"ResourceFileSlotPool"` @ `0x007a0390`, referenced from `FUN_00570f80` @ `0x00570f80`:
  ```c
  void FUN_00570f80(void) {
      DAT_00864f78 = FUN_00440df0(0xf0, 0x50, s_ResourceFileSlotPool_007a0390, 0);
  }
  ```
  This is engine init allocating the `ResourceFile` object pool: size `0xf0` (240 bytes — the PC
  `ResourceFile` object size), count `0x50` (80), named `"ResourceFileSlotPool"`. Directly
  analogous to the GC ctor's `bOMalloc__FP8SlotPool` call. `DAT_00864f78` = PC's
  `g_ResourceFileSlotPool` global.

- `"RegionLoader::LoadHandler"` @ `0x007a0a8c` and `"TrackLoader::LoadHandler"` @ `0x007a0cd8`,
  referenced from `FUN_0057dc60` and `FUN_0057f2d0` respectively. These are **not** the low-level
  `bChunk` dispatcher — they're higher-level per-subsystem streaming *state machines*: each is a
  `switch` on an integer step counter (`param_1[0]`, incremented once per call) that issues one
  resource-file load per step (region data, track geometry, route/path data, dyntex, etc.) via a
  shared `CreateResourceFile`-style helper, in sequence, i.e. the coroutine-style multi-step
  load-a-level flow this engine uses (matches the "load handler" naming pattern seen throughout
  `gc_symbols.txt`, e.g. `AnimBridgeNewDynamicLoader`).

Tracing the helper calls used by both `LoadHandler`s (`FUN_0057cef0`, `FUN_00440df0`, `FUN_0057bd70`,
`FUN_0057cde0`) reproduces the **entire GC `CreateResourceFile`/ctor/`BeginLoading` pipeline on PC,
field offset for field offset**:

| step | GC (`Speed.elf`) | PC (`SPEED2.EXE`) |
|------|-------------------|--------------------|
| slot-pool alloc | `bOMalloc__FP8SlotPool` in `CreateResourceFile` | `FUN_00440df0` called from `FUN_0057cef0` |
| construct + link into global list | `CreateResourceFile__FPCc16ResourceFileTypeiii` @ `0x80195204` | `FUN_0057cef0` @ `0x0057cef0` |
| `ResourceFile` ctor (field init) | `__12ResourceFilePCc16ResourceFileTypeiii` @ `0x80194d2c` | `FUN_0057cde0` @ `0x0057cde0` |
| kick off async load | `LoadResourceFile__...` → `BeginLoading` | `FUN_0057bd70` @ `0x0057bd70` |

The PC ctor (`FUN_0057cde0`) sets **the exact same field offsets** as the GC ctor — `0x08`, `0x0c`,
`0x10`, `0x14`, `0x18`, `0x1c` (path buffer), `0x58`, `0xc8`, `0xcc`, `0xd0`, `0xd4`, `0xd8`
(alloc/flag), `0xdc` (file size), `0xe0`, `0xe4`, `0xe8`, `0xec` — byte-for-byte identical to the
table documented above for GC. This is strong, direct confirmation that `ResourceFile` is literally
the same C++ struct layout on both platforms (shared source, GC/CodeWarrior and
PC/MSVC just happened to preserve member order/packing identically for this class), not just
similar in spirit. `FUN_0057bd70` (`BeginLoading` analog) reads the path from `this+0x1c` and the
file size from `this+0xdc` to kick off the real streamed read — exactly matching GC.

**Not yet found on PC:** the low-level `bChunk`-header/type-dispatch layer (GC's
`EndianSwapChunkHeader`, `FindLoader`, `CallChunkLoader`). Two targeted idiom searches were tried
and came up empty:
- Byte-scan for `AND reg, 0xFC` (the hash-table mask) anywhere in the exe: 366 hits, too broad to
  be useful alone (0xFC is a common immediate for other things, e.g. stack alignment).
- Tightened to "function contains `SHR/SAR ..., 6` **and** `SHR/SAR ..., 12` **and** `AND ..., 0xFC`"
  (the exact shape of GC's `FindLoader` hash `(type + type>>6 + type>>12) & 0xfc`): **0 hits**. MSVC
  most likely codegens the same hash differently (e.g. via `imul`/`lea` instead of two shifts and
  two adds, or the PC hash table uses a different size/mask entirely — plausible since this hash
  table is per-platform engine init, not serialized data). This layer only matters once file bytes
  are already in memory and being walked chunk-by-chunk, so it's lower priority than the
  loading-pipeline mapping above — deferred to a future session as a decompiler-output "shape" search
  (find `FindLoader`'s decompiled *C shape* — small function, one loop, `while(node) if(node->field==key) break else node=node->next`, called from a function with a fallback-array-walk loop) rather
  than a raw instruction-idiom search.

## Empirical chunk-type map (session 4) — supersedes the GC-registration-search approach

Chasing the numeric chunk-type constants through GameCube's `bChunkLoader` registration call
sites (task #13) turned into a dead end: the C++ `bChunkLoader(type, loaderFn, unloaderFn)`
constructor is inlined by the CodeWarrior compiler at essentially every real per-subsystem
registration site (confirmed only one non-inlined call exists in the whole binary, for the
null/sentinel loader — see `gc_ctor_xrefs.txt`), and the individual `LoaderXxx__FP6bChunk` /
`UnloaderXxx__FP6bChunk` function addresses aren't referenced anywhere Ghidra's analysis
recognizes (confirmed via both a `ReferenceManager` xref search and a raw byte-pattern scan of
the whole ELF for each function's absolute address — zero real hits, meaning the registration
table's `{type, loaderFn, unloaderFn}` triples are almost certainly built via split `lis`/`addi`
immediate-load pairs inside inlined code, which Ghidra doesn't always resolve into a data
reference).

Rather than continue down that path, we pivoted to **empirically walking real PC `.BIN` files**
with the header format already confirmed above (`tools/bin_unpack.py`, a reusable
inspector/extractor built from this work). We chunk-walked 646 of the 772 `.BIN` files in
`PC Files/` (recursively, honoring the container top-bit) and correlated each observed `type`
value against the directory/filename it appeared in — this directly gives a type→subsystem
mapping from ground truth, without needing the GC registration code at all:

| type | container? | seen in | meaning |
|------|:---:|---------|---------|
| `0x00000000` | no | everywhere (262 files) | **not actually a terminator** — see below |
| `0x80134000` | yes | 58 `CARS/*/GEOMETRY.BIN` | GEOMETRY: top-level container |
| `0x80034020` | yes | 17 `GEOMETRY.BIN` (smaller/template cars) | GEOMETRY: secondary container |
| `0x00039200`/`01`/`02` | no | same files | GEOMETRY: leaf sub-records (LOD/collision/metadata) |
| `0xb3300000` | yes | 197 `TEXTURES.BIN` | TEXTURES: top-level container |
| `0xb3310000` | yes | 191 `TEXTURES.BIN` | TEXTURES: info sub-container |
| `0x33310001..05` | no | same | TEXTURES: per-texture info leaves (envmap variants at 04/05) |
| `0xb3320000` | yes | 191 `TEXTURES.BIN` | TEXTURES: pixel-data sub-container |
| `0x33320001` | no | same | TEXTURES: small pixel-data leaf (header/mip 0?) |
| `0x33320002` | no | same | TEXTURES: bulk texel data (multi-hundred-KB payload) |
| `0x00e34010`, `0x00037100` | no | 31 `PARTS_ANIMATIONS.bin` | car-part animation leaves |
| `0x80034147` | yes | 113 `TRACKS/.../Paths####.bin` | TRACKS/Paths: top container |
| `0x00034148/49/4a/4c/4d` | no | same | TRACKS/Paths: sub-records (waypoint/spline data) |
| `0x00034121/22/23` | no | 222 `TRACKS/.../Routes####[BF].bin` | TRACKS/Routes: records (B/F = back/front? route variant) |
| `0x00034146` | no | 16 `TrackPosMarkers*.bin` | track position-marker leaves |
| `0x00039000` | no | 14 `LANGUAGES/*.bin` | localized string-table leaf (big, ~41KB) |
| `0x00030201` | no | same | LANGUAGES: per-string-group sub-record |
| `0x00030230/31` | no | `FRONTEND/MAGAZINES_*.BIN` | magazine texture/data leaves |
| `0xb0300100`, `0x30300101/02/03`, `0x00035020/21` | mixed | `FRONTEND/PLATFORMS/Showroom.BIN` | showroom/platform environment setup |

Full raw scan output (all 38 distinct types, file counts, byte offsets) is preserved in the
scratchpad (`pc_bin_types.txt`) for reference if more types need mapping later.

**Type `0x00000000` reinterpreted:** it is not an end-of-stream terminator marker — it carries a
nonzero payload size (e.g. 48, 124, 80, 3134 bytes in different files) and its start offset is
always chosen so the *next* chunk lands on a power-of-two boundary (seen aligning to 0x40 and
0x80 boundaries in `240SX/TEXTURES.BIN`). This matches the GC finding that
`bChunkLoaderFunctionNull` (registered for type 0) just checks `chunk->type == 0` and does
nothing else — it's a generic **padding/alignment filler chunk** that the null loader silently
consumes, not specifically an end marker (though a trailing one can also sit at end-of-file, which
looks like a terminator but is really just final padding).

**Session 7 audit — confirmed the fix generalizes, no other container type has an unhandled
reserved-field quirk.** Ran `bin_unpack.py`'s container walker (with the all-zero-word-is-skippable-
padding fix from the `GEOMETRY.BIN` bug, see `docs/formats/geometry-bin.md`) recursively over every
`.BIN` file under `PC Files/` (772 files). Two checks:
1. Only 3 files produced an "out of bounds" warning, and all 3 are `GLOBAL/*MemoryFile.bin` —
   these are a different, non-`bChunk` serialized-memory-dump format entirely (confirmed by their
   garbage-looking type/size fields), not a real instance of the bug.
2. Across the remaining 768 files, walking every container down to completion and summing
   `(container end offset) - (offset after the last chunk parsed)` gives **exactly 0 leftover bytes
   in every single container, file-wide** (52,090 containers checked) — i.e. there is no other
   container type anywhere in the asset set with stray unaccounted bytes that the current
   zero-skip-and-continue logic might be masking or mis-parsing.

`tools/bin_unpack.py` implements this header/container model as a working, reusable tool: point
it at any `.BIN` and it prints the full chunk tree with type/size/offset and a best-guess label
from the table above, and can optionally dump every leaf's raw payload to disk with `--extract`.

## `ResourceFileType` / top-level file header (task #14) — resolved: there isn't one

Every `.BIN` file walked above has its **first `bChunkHeader` at file offset 0**, with no other
bytes preceding it — confirmed across `TEXTURES.BIN`, `GEOMETRY.BIN`, `PARTS_ANIMATIONS.bin`,
`Routes*.bin`, `Paths*.bin`, `LANGUAGES/*.bin`, and `Showroom.BIN`. There is no container/archive
header, no ID/magic, no chunk count, and no serialized `ResourceFileType` value anywhere in the
file. This means:

- The `ResourceFile` C++ object (whose runtime field layout is documented above) is a purely
  **in-memory** wrapper around a raw chunk stream — none of its fields (`type`, `size`, path,
  list-links, etc.) are read from or written to the `.BIN` file itself.
- `ResourceFileType` is supplied by the **caller** at load time (e.g. `LoadResourceFile(path,
  type, ...)`'s `type` argument comes from whichever subsystem is opening the file — a
  `GEOMETRY.BIN` loader always passes the same constant, a `TEXTURES.BIN` loader always passes
  its own constant, etc.), not read back out of the bytes on disk. Enumerating the actual integer
  values of that enum would require the same GC-registration-callsite search that hit a dead end
  above, and isn't needed to write a working unpacker/converter — the file's own chunk-type tags
  (table above) are self-describing enough for that purpose.
- Practical implication for the 3DS port: a `.BIN` loader only needs to walk the raw chunk stream
  from byte 0 using the `{type:u32, size:u32}` header — no separate archive-header parsing step is
  needed.

## What this means for the `.BIN` archive format on PC

Each `PC Files/CARS/*/….BIN`, `TEXTURES.BIN`, etc. is almost certainly: a `ResourceFile` (whatever
top-level container/header that implies — not yet mapped) containing a flat or nested stream of
`bChunkHeader{type,size}`-prefixed chunks, dispatched at load time by chunk type to whichever
in-game subsystem registered that type. To fully unpack a `.BIN` file we need:

1. The `ResourceFileType` enum values and what top-level file header (if any) precedes the first
   chunk — not yet found.
2. The actual chunk `type` constants (currently only know they're `uint32`, dispatched via that
   hash table — the literal values used by e.g. geometry/texture/vinyl loaders haven't been
   extracted yet).
3. Confirmation that PC `.BIN` files use the *same* chunk-type constants as the GameCube build
   (likely, since this is shared engine code, but unconfirmed — GC is BE, PC is LE, so multi-byte
   fields differ in byte order even if the type IDs are numerically identical).

## Next steps (tracked in HANDOFF.md / task list)

- ~~Find the actual numeric chunk-type constants~~ — done empirically (session 4), see the
  "Empirical chunk-type map" section above. The GC-registration-callsite approach was abandoned as
  a dead end in favor of directly walking real PC `.BIN` files with `tools/bin_unpack.py`.
- ~~Map the `ResourceFileType` enum / top-level file header~~ — resolved (session 4): there is no
  top-level header, see the dedicated section above. Enumerating the actual `ResourceFileType`
  integer values is no longer needed for unpacking and is deprioritized.
- Find `CallChunkLoader`/`FindLoader`'s PC equivalent by decompiled-shape search rather than raw
  instruction idiom (see "Not yet found on PC" above) — needed to confirm PC uses the same
  chunk-type dispatch mechanism at all (very likely, but unconfirmed). Lower priority now that the
  type→subsystem map is known empirically without it.
- Decode the `0x33320002` bulk texel-data leaf's pixel format (likely a DXT/S3TC-family compressed
  format given NFSU2's PC-era DirectX8 rendering, or a raw paletted format — needs a hex/visual
  inspection pass) so texture conversion (Phase 2 of the project plan) can target citro3d's
  supported formats.
- Decode the `GEOMETRY.BIN` leaf payloads (vertex/index buffers, materials) inside the
  `0x80134000`/`0x80034020` containers — currently only the container/leaf *shape* is known, not
  the internal record layout.
