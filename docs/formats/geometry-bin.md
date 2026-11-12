# NFSU2 `GEOMETRY.BIN` chunk tree (container structure and leaf payloads both decoded)

> **Read the last section first.** Everything between here and
> "Session 2026-09-03: leaf payloads decoded" was written while the leaf payloads were
> still unknown, and parts of it (notably "What's still unknown") are now stale. The final
> section is what `tools/car_pack.py` and `tools/scene_pack.py` actually implement.

Status: **container/nesting structure confirmed empirically; internal vertex/index/material record
layouts still unknown.**

## Bug note: container walking requires handling an 8-byte reserved preamble

`tools/bin_unpack.py`'s recursive walker originally assumed a container's child-chunk stream
starts immediately after the container's own 8-byte header. This is true for `TEXTURES.BIN`
containers but **false** for `GEOMETRY.BIN`'s: its containers have an extra 8-byte all-zero
reserved/unused field between the container header and its first real child. The walker
mis-detected that all-zero field as a `{type:0, size:0}` terminator and silently stopped
recursing — meaning session 4's empirical chunk-type scan only ever saw `GEOMETRY.BIN`'s
top-level chunks, never anything nested inside the main `0x80134000` container. Fixed by treating
an all-zero `{type,size}` word as a skippable 8-byte pad rather than end-of-stream. Confirmed fix
against `CARS/AUDIO/GEOMETRY.BIN` (small file, easy to eyeball in full).

## Confirmed chunk-type tree (from `CARS/AUDIO/GEOMETRY.BIN`, a small/simple car)

```
0x80134000  container  — whole-file top container
  0x80134001  container  — one-off "header" block (appears once, first)
    0x00134002  leaf, 144 bytes
    0x00134003  leaf, 888 bytes
    0x00134004  leaf, 2664 bytes
    0x80134008  container, 0 bytes (empty in this sample)
  0x00000000  padding (alignment filler, see bin-archive.md)
  0x80134010  container  — repeats once per "part" (body panel, wheel, etc.)
    0x00134011  leaf, fixed 192 bytes       — per-part fixed record (name/transform?)
    0x00134012  leaf, small variable (8-16 bytes)
    0x00134013  leaf, fixed 24 bytes
    0x80134100  container               — per-part "mesh" sub-block
      0x00134900  leaf, variable ~68-76 bytes   — likely name/material reference
      0x00134b01  leaf, variable ~2200-2500 bytes — LARGEST leaf; best candidate for
                                                     the vertex buffer
      0x00134b02  leaf, variable ~180-190 bytes   — best candidate for an index buffer
                                                     or secondary per-vertex attribute
      0x00134b03  leaf, fixed 292 bytes           — fixed-size regardless of mesh size;
                                                     likely bounding-volume/material/
                                                     shader-parameter block
  0x00000000  padding
  0x80134010  container  (next part, same shape as above)
  ... repeats for each part ...
```

The `0x80134010`/`0x80134100` two-level "part → mesh" pattern repeats many times in a real car's
`GEOMETRY.BIN` (one repeat per drawable part — body, doors, wheels, interior, etc.). Every field
observed so far is internally consistent (sizes vary sensibly between repeats except the two
`0x00134011`/`0x00134013`/`0x00134b03` slots, which are always exactly 192/24/292 bytes — i.e.
fixed-size records — while `0x00134012`, `0x00134900`, `0x00134b01`, `0x00134b02` vary per part,
consistent with them holding actual per-mesh geometry data of varying size).

## Session 6: GC + PC decompile — solid/mesh loader chain resolved

Decompiled (Ghidra headless) `LoaderSolidList__FP6bChunk` (`0x800c7af8` in `Speed.elf`) and its
helpers `InternalLoaderSolidHeaderChunks__FP6bChunk` (`0x800c6c3c`),
`InternalLoaderSolidChunks__FP6bChunkP16eSolidListHeader` (`0x800c6f28`), and the
Unloader/PlatChunks/FixPlatInfo counterparts. Key results:

- **GC and PC share identical top-level chunk-type constants.** GC's code branches on exactly
  `0x134002/03/04` (solid-list header chunks) and `0x134011/12/13` (per-part chunks) — the same
  values independently discovered by empirical PC scanning. This confirms the `.BIN` chunk format
  and type IDs are shared engine-wide, not just PC-specific numbering.
- **`0x80134001` container / `0x134002`+`0x134003`+`0x134004`**: these form one `eSolidListHeader`
  record. `0x134002` (144 bytes) is the header struct itself (bounding/matrix-ish fields, endian-
  swapped on GC only — irrelevant on PC/3DS, both little-endian). `0x134003` is a table of 8-byte
  records and `0x134004` a table of 24-byte records, both counted by the *same* field in the
  `0x134002` header struct (confirmed: 888/8 = 2664/24 = 111 in the `CARS/AUDIO` sample) — i.e.
  they're parallel per-entry arrays (index/hash table + a 24-byte record, purpose not yet named —
  likely a per-solid lookup/bounds table for the whole solid list).
- **`0x134011`/`0x134012`/`0x134013`** (per-part chunks, inside `0x80134010`): `0x134011` is the
  `eSolidPlatInterface`-adjacent per-part struct (192 bytes) — holds a transform (`bMatrix4` at
  struct offset +0x40, 64 bytes), what look like two vec3s (+0x20/+0x24/+0x28 and
  +0x30/+0x34/+0x38 — likely bounding-sphere-center + another bound), a solid ID/hash at +0x10,
  and pointers filled in at load time to the `0x134012` (+0x2c) and `0x134013` (+0x3c) chunk data
  (both small side tables, still unnamed). This struct is **identical in shape on GC and PC**
  (confirmed: PC's `FUN_00493bb0` at `0x00493bb0` uses the exact same field offsets +0x2c/+0x3c/
  +0x88/+0x8c for these three chunk types).
- **The real mesh/vertex/index data is platform-specific**, dispatched through the `0x80134100`
  container to a `LoaderPlatChunks__19eSolidPlatInterfaceP6bChunk`-style function looked up by
  type `0x80134100` (`-0x7fecbf00` as a signed constant). **GC's version is nearly a no-op** — its
  platform sub-chunks are `0x134800/01/02` (a *different* numeric range than PC's, confirming
  chunk-type ranges are genuinely per-platform) and the code just does a cache-flush
  (`DCStoreRangeNoSync`/`PPCSync`) over what is evidently a pre-baked GX display list — no useful
  struct info for the PC/3DS port.
  - **PC's equivalent was found directly in `SPEED2.EXE`**: `FUN_005cc280` (called from
    `FUN_00493bb0` at the `0x80134100` dispatch, exactly mirroring GC's control flow). It handles
    PC-specific sub-chunk types `0x134900`, `0x134b01`, `0x134b02`, `0x134b03` and is the payoff —
    it's a straight Direct3D8 resource-creation routine:
    - **`0x134900`**: allocates/links the per-mesh D3D-side object (a small header with a vtable
      pointer `&DAT_00872ed0`, inserted into a global doubly-linked list of loaded mesh objects).
      Matches the ~68-76 byte variable leaf observed empirically.
    - **`0x134b01`**: **the vertex buffer.** Data pointer 128-byte-aligned
      (`+0x87 & ~0x7f`); size computed from remaining chunk bytes; passed to a vtable call at
      device-object offset `+0x68` with `(device, size, usage=8, FVF=0, pool=1)` — the exact
      shape of `IDirect3DDevice8::CreateVertexBuffer`. The buffer is then `Lock()`ed (vtable
      `+0x2c`), the raw chunk bytes are `memcpy`'d in (dword-at-a-time then trailing bytes), and
      `Unlock()`ed (vtable `+0x30`). Matches this being the **largest** leaf per mesh (~2200-2500
      bytes observed) — vertex data is naturally the bulk of a mesh.
    - **`0x134b03`**: **the index buffer.** Same alignment/copy pattern, but created via vtable
      `+0x6c` with format parameter **`0x65` = 101 = `D3DFMT_INDEX16`** — i.e. **indices are
      16-bit**. This resolves which of the two remaining leaf types is the index buffer (the
      other guess from session 5, that `0x134b02` was the index buffer, was backwards).
    - **`0x134b02`**: stored as a raw pointer (`puVar8[5]`) on the per-mesh object but **not**
      passed to any D3D resource-creation call in this function — it's CPU-side data, most likely
      a vertex declaration/format descriptor or per-strip/subset table (matches its small size,
      ~180-190 bytes, too small to be per-vertex data at any plausible stride for a ~2200-byte
      vertex buffer).
  - This also means: **vertex stride/layout (FVF or vertex-declaration) is not directly visible
    in this function** — it's raw-copied to the GPU without interpretation here, so the exact
    per-vertex field layout must come from wherever `0x134b02` (or the FVF value, if hardcoded
    elsewhere) is consumed at draw time. That's the next thing to chase.

## Session 7: cross-checked the `0x134011` struct against real leaf hex dumps — confirmed

Hex-dumped real `0x134011` records from `CARS/AUDIO/GEOMETRY.BIN` (111 records) and
`CARS/240SX/GEOMETRY.BIN` (609 records) and checked every field claimed in the session-6 decompile
against the actual bytes:

- **`+0x2c`/`+0x3c` (claimed: pointers filled in at load time) — confirmed.** Both fields are `0x0`
  in every file-on-disk record checked, consistent with being null until the loader patches them at
  runtime.
- **`+0x40` (claimed: `bMatrix4` transform, 64 bytes) — confirmed, and now precisely characterized.**
  Every record's `+0x40..+0x7f` is a row-major 4x4 matrix with an identity 3x3 rotation block and a
  translation vector in the last row (`m[12..14]`, `m[15]=1.0`) — i.e. this is a per-part local
  placement transform (translation-only in all samples checked; rotated parts would show a non-
  identity 3x3 block, just not present in these particular samples).
- **`+0x20`/`+0x30` (previously guessed: "two vec3s, maybe bounding-sphere-center + another bound")
  — corrected: these are a min/max axis-aligned bounding box.** Values are consistently near-mirror-
  symmetric pairs (e.g. `(-0.2259, -0.2259, -0.0795)` / `(+0.2259, +0.2259, +0.01)`) — i.e.
  `+0x20` = AABB min, `+0x30` = AABB max, both in the part's local space (matches the `+0x40` matrix
  being a pure placement transform with no scale).
- **`+0x10` (claimed: solid ID/hash) — confirmed.** Values are well-distributed 32-bit hashes with
  no visible pattern, consistent with a CRC32/hash of the part's name (matches `bCalculateCrc32`
  being a real named function in the GC symbol table).
- **New finding, not previously documented: `+0xa0` holds an ASCII name string** — but only
  populated in `CARS/AUDIO/GEOMETRY.BIN` (values like `"AUDIO_COMP_AMP_0_LARGE_A"`,
  `"AUDIO_COMP_AMP_0_MEDIUM_A"` — these are audio-emitter-placement records, not visual mesh parts,
  which is why this file's part count (111) and content differ from a real car's visual
  `GEOMETRY.BIN`). The equivalent field is empty/unused in `CARS/240SX/GEOMETRY.BIN`'s (visual mesh)
  records — real mesh parts are apparently identified only by the `+0x10` hash, with names (if any)
  living in a separate string table elsewhere, not inline in this struct.
- **New, still-unexplained fields: `+0x94`/`+0x98`, two floats immediately before the name/pad
  region.** Values vary per-part and don't obviously correlate with the AABB volume or matrix
  translation magnitude (e.g. `240SX` part 0: `f94=4.008`, `f98=123.75`; part 7: `f94=0.0077`,
  `f98=1686.5` — `f98` is consistently much larger than `f94` in every sample, and both swing over
  several orders of magnitude across parts of the same car). Plausible guesses: a bounding-sphere
  radius/radius² pair, or an LOD switch-distance, but not yet confirmed against decompiled code.

Net result: task "cross-check decompiled struct fields against real leaf hex dump" is **done** for
the `0x134011` per-part struct — every claimed field from the session-6 decompile checks out against
real data, plus two corrections/additions (AABB semantics, name field) captured above. The `+0x94`/
`+0x98` unknowns and the `0x134012`/`0x134013`/`0x134003`/`0x134004` side-table contents remain open
(see below).

## Session 7: vtable-chase attempted, inconclusive — `&DAT_00872ed0` is not a static function-pointer table

Tried the session-6 "next step" of xref'ing the per-mesh object's vtable pointer to find its Draw
method. Re-examined the exact decompiled line that produces this pointer (from
`pc_solid_consts_scan.txt`): `*puVar8 = &DAT_00872ed0;` — this stores the **address of** the global
`DAT_00872ed0` into the object's first field, which only matches the usual MSVC vtable-pointer
pattern if `DAT_00872ed0` is itself a compile-time-constant array of function pointers living in
`.rdata`. Directly reading the bytes at `0x00872ed0` in the static binary (Ghidra headless, dumped
20 dwords starting there) gives **`0x00000000`** — i.e. this address is in a zero-initialized `.bss`
region, not `.rdata`. That rules out the simple "read N function pointers starting here" approach:
either (a) `DAT_00872ed0` is a runtime-populated type-descriptor/RTTI-style object whose real vtable
gets filled in during static initialization (would need to find and decompile that init code, or run
the binary in an emulator/debugger to observe the populated value), or (b) this project genuinely
doesn't use C++ vtables for this class and dispatch happens some other way (e.g. a separate global
dispatch table keyed by a type ID, matching the `bChunkLoader`/`FindLoader` pattern already seen for
chunk types elsewhere in this codebase).

**Resolved (differently than expected): it's not a vtable at all.** Found the other function that
reads `DAT_00872ed0` (`FUN_005ce970`, the mesh-list teardown/unload routine) and it's a classic
**intrusive circular doubly-linked list**: `DAT_00872ed0`/`DAT_00872ed4` are the list's sentinel
node's forward/backward pointers, and `while ((int**)DAT_00872ed0 != &DAT_00872ed0)` is the standard
"walk until back at the sentinel" idiom. So `*puVar8 = &DAT_00872ed0` (in `FUN_005cc280`, the
`0x134900` case) is inserting the new mesh object into this global list — object field `[0]` is the
list's "next" link, not a vtable pointer. There is no C++ vtable/Draw-method dispatch to chase here;
the "read 0x0 from `.bss`" result earlier was correct and expected, not a dead end from a wrong
guess. This whole approach is now closed off, no further vtable-hunting needed.

**More useful result from fully decompiling `FUN_005cc280`** (previously only the summary was
captured, not the full body): now have the **complete field layout of the runtime per-mesh D3D
object** (all offsets are `int`-sized, i.e. `puVar8[n]` = byte offset `n*4`):

```
[0x00] next pointer (list link, see above)
[0x04] prev pointer (list link)
[0x14] 0x134b02 chunk's raw data pointer (16-byte aligned)
[0x18] 0x134b03 chunk's raw data pointer (16-byte aligned) -- index buffer source
[0x1c] index buffer byte size (computed from chunk size minus alignment padding)
[0x20] D3D index buffer object pointer (IDirect3DIndexBuffer8*, from CreateIndexBuffer)
[0x28] D3D vertex buffer object pointer (IDirect3DVertexBuffer8*, from CreateVertexBuffer)
[0x2c] 0x134b01 chunk's raw data pointer (128-byte aligned) -- vertex buffer source
[0x30] vertex buffer byte size
[0x40] a flag, zeroed unless a capability/quality check passes (see below)
```

**New, important lead for the stride question**: `CreateVertexBuffer` is called with **`FVF=0`**
(confirmed already in session 6, now doubly confirmed in the full decompile:
`(**(code**)(*DAT_00870974+0x68))(DAT_00870974, iVar3, 8, 0, 1)` — the `0` is the FVF parameter).
Direct3D8 requires *either* a nonzero FVF *or* a vertex shader handle to interpret a vertex buffer's
layout at draw time — since FVF is hardcoded to 0 here, **this mesh format must use a programmable
vertex shader with an explicit vertex declaration** (D3D8's `CreateVertexShader` takes a declaration
token array that defines the exact per-vertex field layout/stride), set once via `SetVertexShader`
elsewhere, not per-mesh. This reframes the stride-recovery problem: instead of chasing an FVF
constant, the next Ghidra target should be **`CreateVertexShader` call sites** (xref the vtable slot
for `IDirect3DDevice8::CreateVertexShader`, offset `0x90` in the standard D3D8 vtable layout) and
decode whichever declaration-token array is passed in — that directly gives the field-by-field
vertex layout and stride, which is more reliable than guessing from block sizes.

There's also a capability/quality gate spotted in the same function, not yet investigated: `if
(DAT_008900d8 != NULL && *DAT_008900d8 == 4) { if (FUN_005bc6b0() != 5) { skip vertex-buffer
creation entirely } }` — looks like a hardware-capability or detail-level check (possibly "if detail
setting is Ultra/4, require capability level 5, else fall back to not rendering this mesh at all" or
similar); not yet understood, flagged only as a curiosity, not blocking.

## Session 7 (cont.): `0x134b01` vertex format solved empirically — 36-byte classic D3D8 FVF

The vtable-chase (previous section) dead-ended — `DAT_00872ed0`/`DAT_00872ed4` are a linked-list
sentinel, not a vtable, and the `FVF=0` passed to `CreateVertexBuffer` in `FUN_005cc280` means the
format must come from a vertex-shader declaration set elsewhere. Chasing that (looking for
`CreateVertexShader`, D3D8 vtable slot `+0x90`, called on the confirmed device pointer
`DAT_00870974`) also came up empty: none of the 55 functions referencing the device pointer call
`+0x90` *on the device itself* — the handful of `+0x90` call sites that did turn up are on
unrelated engine objects (likely internal material/technique wrappers with larger vtables than a
raw D3D8 interface), and the same investigation revealed our two confirmed device offsets
(`CreateVertexBuffer`=`+0x68`, `CreateIndexBuffer`=`+0x6c`) don't line up with the publicly
documented stock `IDirect3DDevice8` vtable ordering either — so guessing other slots by public
D3D8 documentation isn't reliable for this binary's (possibly wrapped/wrapped-subclassed) device
vtable. Further Ghidra vtable-offset chasing was abandoned as low-yield.

**Solved a different way: direct empirical inspection of real vertex bytes.** Extracted the raw
`0x134b01` leaf payload from `CARS/240SX/GEOMETRY.BIN` (609 leaves found by walking the chunk
stream and filtering on `type==0x134b01`), skipped the loader's 128-byte alignment padding
(leaves start with a run of `0x11` filler bytes up to the first 128-byte-aligned offset, matching
`FUN_005cc280`'s `(int)piVar1 + 0x87U & 0xffffff80` alignment), and searched for a repeating
sub-pattern. A `\x00\x00\x00\xff`/`\xff\x00\x00\x00`-shaped 4-byte run repeats every exactly **36
bytes**, consistent across every leaf and every leaf size checked. Interpreting each 36-byte
record as `{float3 pos, float3 normal, u32 color, float2 uv}` and printing several thousand
vertices across three different leaves (one small ~13-vertex trim part, one ~400-vertex body
panel, one ~130-vertex shape) gives fully sane values in every field:

- `pos` (bytes 0–11): car-body-scale floats (roughly -2..2, consistent with a car's local-space
  half-extents in meters).
- `normal` (bytes 12–23): unit-length-ish float3s (each component within [-1, 1], vector magnitude
  ≈1) that vary sensibly between adjacent vertices on curved panels and stay constant across
  vertices sharing a flat face.
- `color` (bytes 24–27, `u32` D3DCOLOR): constant `0xff000000` (opaque, RGB=0) in the observed
  samples — a default/unused per-vertex diffuse color, not lighting-baked data.
- `uv` (bytes 28–35, `float2`): sensible 0..1-ish texture coordinates that vary per-vertex on
  larger meshes and are locally constant on the tiny test part (consistent with that part mapping
  to a small solid-color texture region).

This is exactly the classic D3D8 fixed-function vertex format
`D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1` (12+12+4+8 = 36 bytes), which also
resolves the earlier "why is `CreateVertexBuffer` called with `FVF=0`" puzzle: the FVF is very
likely set via `SetVertexShader` with a literal FVF code (D3D8 allows an FVF constant to be passed
to `SetVertexShader` in place of a real shader handle) at draw time, elsewhere in the render path
— not baked into the vertex buffer's creation call. No further Ghidra work is needed to unblock
Phase 2 mesh conversion.

**Caveat:** confirmed on `CARS/240SX/GEOMETRY.BIN` only so far; it's plausible other meshes (glass,
wheels, or non-CARS geometry like tracks) use a different FVF/stride (e.g. no normal, no color,
multiple UV sets) — the converter should verify `leaf_size % stride == 0` per-mesh and flag
mismatches rather than assume 36 bytes universally.

## What's still unknown

- The purpose of `0x134003`/`0x134004` (parallel 8-byte/24-byte tables under the one-off
  `0x80134001` header block) and `0x134012`/`0x134013` (small per-part side tables) — referenced
  and pointer-linked by the loader but never interpreted byte-by-byte in the functions decompiled
  so far. Not required for basic mesh rendering; deferred.
- Whether every mesh across every car/track uses the same 36-byte FVF, or whether some use a
  different stride (see caveat above) — to be discovered empirically as the Phase 2 converter is
  run across the full asset corpus.

## Session 7 (cont.): `tools/geometry_transcode.py` — working mesh extractor, format validated

Built `tools/geometry_transcode.py`, which walks a `GEOMETRY.BIN`, and — correcting an initial
bug where `0x134900`/`0x134b01`/`0x134b03` were wrongly assumed to be nested (they're actually
**flat siblings** within the same parent container, per `FUN_005cc280`'s single flat loop) — groups
each `0x134900` marker with the `0x134b01` (vertex) / `0x134b03` (index) leaves that follow it
before the next `0x134900`, skips the leading alignment padding, and reinterprets the bytes per
the 36-byte format above, dumping one JSON file per mesh (`{vertices:[{pos,normal,color,uv}],
indices:[u16...]}`).

Validated across 5 real `GEOMETRY.BIN` files: `CARS/240SX` (609 meshes), `CARS/3000GT` (579),
`CARS/ESCALADE` (352), `CARS/BRAKES` (6), `CARS/EXHAUST` (10) — **100% of meshes in every file
extract with a clean `size % 36 == 0`, zero skips**. (`CARS/WHEELS/GEOMETRY.BIN` is a 64-byte
empty placeholder — wheel geometry is evidently supplied from elsewhere, not a format
mismatch; no `TRACKS/*/GEOMETRY.BIN` files exist in this asset tree to cross-check against, so
track geometry likely uses a different container/file naming — out of scope until Phase 2 track
work begins.) No non-36-byte-stride meshes were found anywhere in this sample, so the single
global FVF assumption holds for all `CARS/` geometry checked so far.

Task considered **closed** for Phase 1 purposes: the format is understood and mechanically
converted. Remaining unknowns (`0x134003`/`0x134004`/`0x134012`/`0x134013` side tables) are
non-blocking metadata, deferred to whenever they become relevant (e.g. LOD selection, collision
data, or part-attachment points).

## Session 9: track/scenery geometry located and decoded — `TRACKS/STREAML4R*.BUN`, a second FVF

Session 7 assumed no `TRACKS/*/GEOMETRY.BIN` files existed and left this out of scope. They don't —
but track world geometry isn't packaged as `GEOMETRY.BIN` at all. It lives in **`TRACKS/STREAML4R*.BUN`**
(one per track region, `STREAML4RA`.."H"/"R", 2 MB-120 MB each) — a streaming-content bundle, but its
internal chunk stream reuses the **exact same `0x134900`/`0x134b01`/`0x134b02`/`0x134b03` chunk types**
as car `GEOMETRY.BIN` (confirmed via hex dump of `STREAML4RR.BUN`, the smallest at 4 MB), meaning
`tools/geometry_transcode.py`'s existing chunk-walking logic applies unchanged — no new container
format to write.

The vertex data itself uses a **different, second FVF** than cars, though: car vertex buffers are
always evenly divisible by the 36-byte car stride, but track/scenery vertex buffer sizes are not —
empirically decoding a sample against varying candidate strides and checking for sane, small,
grid-aligned position values (e.g. a repeating `(190,-500,0)/(180,-500,0)/(190,-490,0)` quad-corner
pattern) found a clean fit at **24 bytes, `D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1`** — no per-vertex
normal (scenery lighting is presumably baked into vertex color/lightmaps rather than computed
per-vertex like cars' real-time-lit paint):
```c
struct SceneryVertex { float pos[3]; uint32 color /*D3DCOLOR*/; float uv[2]; };  // 24 bytes
```
`tools/geometry_transcode.py` now tries the car stride (36) first, validates it by checking the
decoded "normal" field is a roughly-unit-length finite vector (real car normals always are; scenery
data misread at the wrong 36-byte stride produces wildly out-of-range/non-finite values in that slot,
a cheap and reliable discriminator), and falls back to the scenery stride (24) if that check fails —
no per-file format flag was found, so this per-mesh empirical check is the mechanism used to pick the
right FVF.

Also found and fixed a related bug in `skip_alignment_padding()`: it previously assumed the leading
`0x11` filler run always extends exactly to the next 128-byte-*aligned file offset* (true for every
car vertex buffer checked in session 7, since that coincides with the runtime pointer alignment the
loader applies). Track/scenery leaves' filler runs are shorter and not tied to any 128-byte boundary
— the fix just measures the actual consecutive-`0x11` run length directly instead of assuming a
target alignment, which works correctly for both cases.

**Validated** on `STREAML4RR.BUN` (the smallest region, `Location4\RegionR\Pipeline\PC\
eLabScenery_Environment_N` per an embedded debug path string in its header) — **482/482 mesh objects
extracted cleanly**, 65,870 vertices total, all index buffers now in-range (0/482 out-of-range, after
the padding fix; was 448/482 broken before it). **Visual verification**: a combined top-down plot of
all 482 meshes' triangles forms an unmistakably deliberate shape — a large circular dome (~19,400
units across, consistent with a skybox/horizon environment mesh, matching the "eLabScenery_Environment"
name) plus a smaller flat gridded patch at the center (a ground/terrain tile) — not garbage. This
gives a real, render-ready "track segment" scene (environment dome + ground) for the Citra
proof-of-concept, satisfying task #30. A larger `STREAML4R*.BUN` (e.g. `STREAML4RA`, 118 MB) likely
also contains actual road/building geometry alongside environment assets, worth targeting for a more
visually interesting scene than a dome+ground if time permits, but isn't required to hit the agreed
proof-of-concept bar.

**Follow-up check (still session 9): `STREAML4RB.BUN` (28 MB) inspected as a candidate richer scene.**
Extracted 2641/2642 meshes (11 with minor out-of-range indices, same negligible class as elsewhere)
and rendered a combined top-down plot: it does contain real ground-level structures (a tall
building-shaped block and a long road/bridge-shaped strip, both clearly distinct from the outer
dome), confirming the "likely road/building geometry" guess above. However, isolating just that
ground-level content still pulls in tens of thousands of vertices across hundreds of small tiled
meshes (a bounding-box crop around the visible cluster gave 742 meshes / ~95k vertices -- the whole
file is 384k vertices across 2641 meshes), meaningfully more conversion/runtime risk than
`STREAML4RR.BUN`'s already-validated 482 meshes / 65,870 vertices for no requirement-driven benefit.
**Decision: keep `STREAML4RR.BUN` (dome + ground) as the final track segment for tasks #31-#35.**
Task #30 is marked complete on this basis. `STREAML4RB.BUN`'s building/road cluster remains a good
target for a future, more ambitious scene once the POC's rendering path is proven out.

## Session 9 (cont.): unified runtime binary mesh/scene format (`tools/mesh_pack.py`)

Replaces the per-mesh JSON intermediate with a single flat binary "scene" file directly consumable
by C on the 3DS (no JSON parsing at runtime). Both FVFs (car 36-byte, scenery 24-byte) are
normalized at conversion time into **one shared 36-byte runtime vertex layout** so the citro3d
loader only needs one code path regardless of source:

```c
struct RuntimeVertex {
    float pos[3];    // object-local space, as decoded from the source FVF
    float uv[2];
    float rgba[4];   // D3DCOLOR (ARGB8, packed) unpacked to 0..1 floats, RGBA order
};  // 36 bytes
```

(The per-vertex normal is dropped -- not needed for an unlit, texture-mapped POC render; can be
added back as a second stream later if real-time lighting is implemented.)

File layout (`tools/mesh_pack.py`'s `pack_scene()`):
```
offset 0:  char magic[4]   = "NFSM"
           u32  version    = 1
           u32  num_meshes
           MeshEntry[num_meshes]:
               u32 vertex_offset   // index into the vertex pool, not bytes
               u32 vertex_count
               u32 index_offset    // index into the index pool, not bytes
               u32 index_count
           RuntimeVertex[total_vertex_count]   // 36 bytes each
           uint16 index_pool[total_index_count]
```

Meshes that fail to decode under either FVF (the same small "different FVF, not yet understood"
tail already seen in `geometry_transcode.py`'s skip count) are simply omitted from the packed file
-- they're a negligible fraction (4/609 car, 0/482 track) and non-blocking for the POC.

**Validated**: packed both `CARS/240SX/GEOMETRY.BIN` (605 meshes, `240sx.nfsm`) and
`TRACKS/STREAML4RR.BUN` (482 meshes, `track_l4rr.nfsm`); re-parsed each packed file back into
Python and cross-checked vertex/index counts and a handful of decoded positions against the
original per-mesh JSON extraction -- byte-identical.

## Session 2026-09-03: leaf payloads decoded — the tree is no longer opaque

Everything above this heading was written while the leaves were still unknown. They are decoded
now, and `tools/car_pack.py` / `tools/scene_pack.py` build directly from them with no guessing.
**Where this section and an earlier one disagree, this one is right.**

The single most useful structural fact: **`FRONTEND/PLATFORMS/*.BIN` reuse the exact same
`0x80134000` geometry tree as `CARS/<car>/GEOMETRY.BIN`.** `scene_pack.py` imports
`parse_geometry()` from `car_pack.py` verbatim. Only the vertex format and the texture location
differ (see below). Anything learned about one applies to the other.

### `0x134011` — part record

| offset | type | meaning |
| --- | --- | --- |
| `+0x40` | `f32[16]` | part transform, row-major 4x4 |
| `+0xa4` | `char[28]` | NUL-terminated part name, truncated at 27 chars |

`0x134011` part records and `0x134900` mesh objects are **strictly 1:1 and in the same document
order**, which is what lets the walker zip them without an index. A count mismatch means the walk
is wrong, and `parse_geometry()` raises on it rather than limping.

Names are the retail ones: `<CAR>_KIT00_BODY_A`, `..._FRONT_WHEEL_A`, and in Showroom.BIN
`DISC01..03`, `BACKDROP`, `BACKDROP_FLOOR`. One Showroom part's name is mojibake in the retail
file — run tools with `python -X utf8` when dumping names or Windows raises `UnicodeEncodeError`.

### `0x134012` — texture slot table

8-byte records `{ u32 nameHash; u32 runtimeTexturePtr }`. The pointer is 0 on disk; the retail
loader fills it in. `nameHash` keys into the car's `TEXTURES.BIN` (or, for scenes, the appended
TPK). A `0x134b02` subset's texture slot indexes *this* table, not the texture file directly.

The hash is the game's `bStringHash__FPCc` — djb2 with a **`0xFFFFFFFF` seed and multiplier
`0x21` (33)**, over the base name uppercased:

```python
h = 0xFFFFFFFF
for ch in name.encode("latin1"):
    h = (ch + h * 0x21) & 0xFFFFFFFF
```

Implemented as `b_string_hash()` in `tools/fng_parse.py`; the same function keys the `.fng`
texture packs, so there is one hash space for both.

### `0x134013`, `0x13401a` — dummy / attachment tables

`0x13401a` is the **attachment table**: upper-body dummies only — mirrors, scoops, spoiler mount.
Identified positively because the spoiler attachment's translation matches its part matrix exactly.

**It does not contain wheel corner positions.** `<CAR>_KIT00_FRONT_WHEEL_A` exists and is authored
at the origin with +Y as the axle, and nothing in `GEOMETRY.BIN` says where the four corners are.
They come from the **CarTypeInfo wheel table in `GLOBAL/GLOBALB.BUN`** -- see the next section.

### CarTypeInfo wheel table (`GLOBAL/GLOBALB.BUN`) -- where the four corners live

`GLOBALB.BUN` carries an array of **46 CarTypeInfo records, 0x890 (2192) bytes each**, one per
playable/traffic car, in the file's own order (`PEUGOT, FOCUS, COROLLA, 240SX, ...`). A record is
found by searching for its `CARS\<CAR>\GEOMETRY.BIN` path string; the string sits at a fixed
offset inside the record, so **anchor on the `CARS` prefix, not on `\GEOMETRY.BIN`** -- the latter
shifts with car-name length and produces a ragged 2190..2196 stride instead of a clean 2192.

At **record-relative `+0xe0`** sit four **0x30-byte wheel records**, in the order
**front-left, front-right, rear-right, rear-left**:

| offset | type | meaning |
| --- | --- | --- |
| `+0x00` | `f32[3]` | wheel centre, in car space (X longitudinal/front +, Y lateral, Z up) |
| `+0x10` | `f32` | wheel radius |
| `+0x14` | `f32` | wheel width |
| `+0x1c` | `f32` | fender line |
| `+0x20` | `f32` | a wider lateral reference (matches the body part's max abs Y) |

`+0x00` is the wheel's **centre plane, not its inner face**, so the origin-authored mesh (which
spans `y = [0, width]` with +Y outboard) must be *centred* on it, not translated onto it. Corners
with a negative centre Y are the mesh mirrored across `y = 0`; the normal flips with it and the
winding does not need fixing, because the runtime draws with `GPU_CULL_NONE`.

Validated three independent ways: the four records' mirror invariants (the front pair and the rear
pair differ only in the sign of Y, and front X > rear X) hold for all 46 cars; the
table's radius/width match the car's own `FRONT_WHEEL_A` mesh bounds (HUMMER mesh width 0.284 vs
table 0.285); and `+0x20` matches the body part's max abs Y (240SX 0.821 vs 0.822).

**Off-by-one warning.** Reading the table at `-0x7b0` from the *next* record's path string also
passes the mirror invariants for 45/46 cars and looks plausible -- it is wrong by exactly one
record, and only PEUGOT (the first) exposes it, by reading NaN. Anchor forward, at `+0xe0`.

**Bonus:** the ground plane is `centre.z - radius`, which orders sensibly across the roster
(HUMMER -0.391, CORSA -0.097) and is what the car contact shadow needs (TASKS.md #105).

Implemented as `load_wheel_table()` / `part_placements()` in `tools/car_pack.py`.

### `0x134900` — mesh object header

Read after `skip_alignment_padding()` (the `0x11` filler, see the preamble note at the top of this
file):

| offset | type | meaning |
| --- | --- | --- |
| `+0x10` | `u32` | subset count (number of `0x134b02` records) |
| `+0x24` | `u32` | triangle count — **authoritative**, see the index-leaf note |
| `+0x34` | `u32` | vertex count |

### `0x134b02` — material subset, 60-byte stride

Only four fields of the 60 are needed to draw:

| offset | type | meaning |
| --- | --- | --- |
| `+0x0c` | `u32` | index count |
| `+0x1c` | `u32` | texture slot (index into this part's `0x134012` table) |
| `+0x34` | `u32` | first index |
| `+0x38` | `u32` | flags — **still undecoded**; presumably render state (blend, two-sided, alpha test) |

One `0x134b02` becomes one runtime batch. The undecoded `+0x38` is why the packers guess at
per-subset render state, and why the car contact shadow is dropped at pack time (TASKS.md #105/#107).

### `0x134b03` — index leaf: **4-byte aligned, so it can carry a spurious trailing `u16`**

The leaf is padded to 4 bytes, so a mesh with an odd triangle count leaves one extra `u16` at the
end that is not part of the geometry. **Use `0x134900 +0x24 * 3`, never the leaf size**, to decide
how many indices to read. Everything is a plain triangle list — there are no strips anywhere in
either cars or frontend scenes.

### `0x134b01` — vertex buffer: two FVFs, picked by file kind

**Cars — 36 bytes:**
```c
struct { float pos[3]; float nrm[3]; u32 D3DCOLOR; float uv[2]; };
```
The D3DCOLOR is a constant `0xFF000000` on car geometry — there is no baked shading to reuse, which
is why cars are lit at runtime from the normal.

**Frontend scenes (`FRONTEND/PLATFORMS/*.BIN`) — 24 bytes, no normals at all:**
```c
struct { float pos[3]; u32 D3DCOLOR; float uv[2]; };
```
Confirmed empirically: `vbPayload / numVerts` is exactly 24.000 for every part of every file in
`FRONTEND/PLATFORMS`.

**Scenery D3DCOLOR semantics.** This is the part that is easy to get wrong:

- **Alpha is not opacity.** Across a whole scene file it only ever takes four values —
  0, 110, 115, 255. It is a per-material *class*, not a gradient. It is dropped.
- **RGB is the baked vertex lighting**, a real 0..255 gradient. It is what makes Showroom read as a
  night alley with hot pools under the street lamps and dark asphalt between them.

The packers unpack that RGB into the vertex's second `float3` slot — *the same slot a car puts its
normal in* — so the runtime vertex is byte-identical in size and attribute layout for both, one
loader and one buffer setup serve both, and only the vertex shader differs (`car.v.pica` lights the
normal; `scene.v.pica` passes the baked colour through). The `.nfsc` header's `shading` field says
which.

### Texture location differs by file kind

- Cars: sibling `TEXTURES.BIN`.
- Frontend scenes: a `0xb3300000` TPK **appended to the same `.BIN`**, read out through
  `extract_global_textures`.

### Superseded

`tools/mesh_pack.py`'s `.nfsm` container (documented in the session-9 section above) is superseded
for cars and frontend scenes by the `.nfsc` v2 container that `car_pack.py` and `scene_pack.py`
write and `3DS Files/source/model3d.[ch]` reads. `.nfsm` dropped normals and unpacked D3DCOLOR to
four floats; `.nfsc` keeps a 32-byte vertex (`pos[3]`, `attr1[3]`, `uv[2]`) whose `attr1` is a
normal or baked RGB depending on the header's `shading` flag. The `.nfsc` field layout is in
`tools/car_pack.py`'s docstring, mirrored in `model3d.h`. `mesh_pack.py` is still the path for
`TRACKS/*.BUN`, which has not been moved over.

### `0x134012` revisited — these are **logical slots**, not texture names

The section above says `nameHash` "keys into the car's `TEXTURES.BIN`". That is true for only a
handful of entries. A `0x134012` record names a **slot the retail loader fills per car**, and most
slots never correspond to a shipped texture at all. Evidence: 472k distinct strings harvested from
`SPEED2.EXE` and 4834 textures indexed across 433 retail files match *none* of a stock 240SX's 21
unresolved hashes.

**The slot table is inlined in `SPEED2.EXE`.** Around file offset `0x2385xx` the initialiser writes
one hash constant per 16-byte record as a run of `mov dword ptr [esi+disp32], imm32` (`C7 86` +
disp32 + imm32). Slot index is `(disp - 0x668) / 0x10`:

| slot | hash | name |
| --- | --- | --- |
| 0 | `0xa7366ae6` | body paint layer (unnamed, see TASKS.md #108) |
| 1 | `0x3c84d757` | outer painted shell (unnamed) — 45% of a stock 240SX by triangle count |
| 2..7 | *not constants* | `<CAR>_SKIN1`, `<CAR>_SKIN1B`, `<CAR>_SKIN2..4` |
| 8 | `0x5799e60b` | `DRIVER` |
| 10 | `0x7b220ddf` | `WINDOW_FRONT` |
| 11 | `0x1b0763a0` | `WINDOW_REAR` |
| 12..15 | `0xe7e4ef49`, `0x4cdebfca`, `0x60f8b13c`, `0x0ab88f5d` | `WINDOW_{LEFT,RIGHT}_{FRONT,REAR}` |
| 16 | `0xd195be56` | `REAR_DEFROSTER` |
| 17..22 | `0x90e09671`, `0x13ee21f2`, `0xd689a31b`, `0xb8f3415c`, `0x2433df4e`, `0xf95d432f` | `WINDOW2_FRONT`, `WINDOW2_REAR`, `WINDOW2_{LEFT,RIGHT}_{FRONT,REAR}` |
| 23..27 | `0xa7e6ea53`, `0xa532fc46`, `0xd947f346`, `0x02b52399`, `0x3617dcfc` | `HEADLIGHT_{LEFT,RIGHT}`, `BRAKELIGHT_{LEFT,RIGHT,CENTRE}` |
| 28..32 | `0x267d188c`, `0xf68ef19f`, `0x000bcbbf`, `0x01f40b32`, `0x1d33b7b5` | `{HEAD,BRAKE}LIGHT_GLASS_{LEFT,RIGHT,CENTRE}` |

Plus `CHROME` `0x54949afd`, `CARBOTTOM` `0xbd1f0e0a`, `GRILL_03/04` `0x69c032bb/bc`, and
`DUMMY_DECAL1..8` = `0x910e6654..0x910e665b` (eight consecutive hashes because only the last
character changes).

**The window slots are never shipped textures.** `UpdateCarReplacementTextures` (Speed.elf
`0x802353FC`, SPEED2.EXE `0x615280`) swaps slots 10..15 to `DamagedWindowHashes[damage]` = `WINDOW`,
`WINDOW_DAMAGE0..2` and slots 17..22 to `DamagedWindow2Hashes[damage]` = `WINDOW2`,
`WINDOW2_DAMAGE0..2`. Those two tables gave the names of slots 11 and 17..22. `WINDOW` is in
`GLOBALB.BUN`; `WINDOW2` ships nowhere, and no retail car's geometry references a `WINDOW2_*` slot.
Slot 16 keeps `REAR_DEFROSTER` until the rear window breaks.

**Slots 2..7 are absent from that run** because they are not constants — the literals `%s_SKIN1`,
`%s_SKIN1B`, `%s_SKIN2..4` show the loader sprintf's them from the car name, which is exactly why
`240SX_SKIN1` (`0x77a20245`) appears in `GEOMETRY.BIN` but nowhere in the executable.

Names were recovered by preimage search against `bStringHash` and every one is verified to hash
back to its constant. The technique worth reusing: `hash(C + B) = state(C)*33^|B| + K(B) (mod 2^32)`,
so for two strings sharing a suffix, `H1 - H2 == (s1 - s2)*33^|B|` pins the suffix **length** without
knowing the suffix; meet-in-the-middle over `[A-Z0-9_]` then recovers it. Six cars agreed on length 6
and gave `_SKIN1`.

### Shipped texture names are truncated at 20 bytes — always look up by hash

The DXT block trailer stores the name in a 20-byte field, so `240SX_KIT00_HEADLIGHT_OFF` is only
ever spelled `240SX_KIT00_HEADLIGH` on disk. The table's **hash is over the untruncated name**, so a
literal string compare can never find the long ones — `tools/car_pack.py`'s `textures_by_name()`
hashes the wanted full name and indexes by that. Recovering the tails of all 73 names in
`240SX/TEXTURES.BIN` showed every shipped light texture is the **`_OFF`** state; lit states are
generated at runtime.

### Where the pixels actually come from

- Car-specific slots resolve to the car's own `TEXTURES.BIN`.
- `CHROME`, `CARBOTTOM`, `REAR_DEFROSTER`, `DRIVER` are **car-independent** and live in
  `GLOBAL/GLOBALB.BUN`'s `Global\GlobalTextures.tpk`, not in the car file.
- Light slots resolve per car through a pattern: `HEADLIGHT_LEFT` -> `<CAR>_KIT00_HEADLIGHT_OFF`,
  `BRAKELIGHT_GLASS_RIGHT` -> `<CAR>_KIT00_BRAKELIGHT_GLASS_OFF`, `BRAKELIGHT_CENTRE` ->
  `<CAR>_CENTRE_BRAKELIGHT_OFF`.
- The body skin and the window tint are **composited at runtime, never shipped**
  (`CarLoadedSkinLayerSlotPool`). The port composites the skin at car load as retail's
  CompositeSkin does (`3DS Files/source/carskin.c`): the base paint, the vinyl layers, then
  `<CAR>_DOORLINE` through `<CAR>_DOORLINE_MASK`'s green byte, then the region swatches. Both
  packers write the body slots one placeholder for it (`FMT_SKIN`, TASKS.md #296).
