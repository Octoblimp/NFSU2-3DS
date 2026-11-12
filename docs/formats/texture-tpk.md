# NFSU2 texture pack (`.tpk` / `TEXTURES.BIN` pixel data) format

Status: **car (`TEXTURES.BIN`) and frontend/global (`GLOBALB.BUN`-embedded `.tpk`) variants fully
decoded and extractable; track (`TRACKS/*.BUN`) variant still undecoded** (see session 12 below).
Found while investigating the `0x33320002` bulk texel-data leaf inside `TEXTURES.BIN` (see
`docs/formats/bin-archive.md`'s empirical chunk-type map).

## Chunk-level structure (confirmed, `CARS/240SX/TEXTURES.BIN`)

Inside the `0xb3310000` info container:
- `0x33310001` (124 bytes): package name/path record — ASCII `"CARTEXTURES"` then a path string
  `Global\Pipeline\CarTemplateTextures_240SX.tpk` — i.e. the original PC pipeline's on-disk `.tpk`
  filename is preserved as metadata even though the actual bytes are embedded directly in
  `TEXTURES.BIN`, not loaded from a separate `.tpk` file at runtime.
- `0x33310002` (584 bytes = 73 × 8-byte records): per-texture hash/id table — each entry looks
  like `{ hash: u32, flags/pad: u32 }` (e.g. `0a48d102 00000000`).
- `0x33310003` (1752 bytes = 73 × 24-byte records): per-texture metadata table, same leading hash
  as the `0x33310002` entry it corresponds to, followed by additional fields (offsets, dimensions,
  and/or a compressed/uncompressed size pair — not yet individually decoded field-by-field, but the
  73-entry count exactly matches `0x33310002`, confirming these are parallel arrays keyed by the
  same per-texture hash).

Inside the `0xb3320000` pixel-data container: `0x33320001` (24 bytes, small — likely a
count/summary header) followed by `0x33320002`, one big leaf holding **all 73 textures'
compressed pixel data concatenated**.

## Pixel-data leaf (`0x33320002`) — EA "JDLZ" compression confirmed

Extracting a real `0x33320002` leaf (`tools/bin_unpack.py --extract`) and hex-dumping it shows,
after 0x78 bytes of `0x11`-repeated filler (probably an unused/padding slot — not yet explained,
possibly alignment or a deliberately-blanked first table slot):

```
...78 bytes of 0x11 fill...
4a44 4c5a 0210 0000 1c2b 0000 4c16 0000 82ff 0000 04b3 8cce 8baa 0000 ...
'J'  'D'  'L'  'Z'  ^^ver  ^^^^^^^^^ ^^^^^^^^^ ^^^^^^^^^ <compressed stream...>
```

`4A 44 4C 5A` = ASCII `"JDLZ"` — this is EA Blackbox's proprietary LZ-family compressor, used
across the NFS Underground/Most Wanted/Carbon/ProStreet/World-era EAGL-engine titles (confirmed
via public documentation/open-source decoders, not guessed). Per known-good documentation of the
format, the 16-byte header is:

```c
struct JDLZHeader {
    char     magic[4];        // "JDLZ"
    uint8_t  ver_major;       // 0x02 observed
    uint8_t  ver_minor;       // 0x10 observed
    uint16_t reserved;        // 0x0000 observed
    uint32_t uncompressed_len; // little-endian
    uint32_t compressed_len;   // little-endian, includes this 16-byte header
};
```

Our sample: `uncompressed_len = 0x2b1c` (11036), `compressed_len = 0x164c` (5708, header
included) — i.e. this is the **first of 73 back-to-back JDLZ blocks** packed into the
`0x33320002` leaf (the leaf is ~937KB total; one texture's compressed block is ~5.7KB), each
presumably addressable via the offset/size fields in the still-not-fully-decoded `0x33310003`
per-texture metadata table.

The algorithm itself is a lightweight, fully-sequential LZ variant (reportedly LZRW1/1a-derived).
**Ported and confirmed working** (session 5): `tools/jdlz.py`, ported directly from
[`nfs-tools/tpktool`](https://github.com/nfs-tools/tpktool)'s `JDLZ.cpp` (itself ported from
[`MWisBest/OpenNFSTools`](https://github.com/MWisBest/OpenNFSTools)'s `LibNFS/Compression/JDLZ.cs`).
Successfully decompressed the first block of `CARS/240SX/TEXTURES.BIN`'s `0x33320002` leaf
(`compressed_len=0x164c` → `uncompressed_len=0x2b1c`, matching the header's stated lengths exactly)
with no errors or garbage output.

## Pixel encoding — confirmed: DXT3 (S3TC), with a per-texture trailer

Decompressing a real block reveals the layout is **not** just raw pixels — it's pixel data
followed by a small metadata trailer, both produced by the same JDLZ block:

```
[0 .. pixel_data_size)      : raw DXT3-compressed block data
[pixel_data_size .. end)    : trailer (~156 bytes in the sample):
    - texture name (ASCII, e.g. "240SX_BADGING")
    - a hash/CRC (u32)
    - several u32 fields — sizes/offsets, still only partly decoded (one field's value
      exactly equals `pixel_data_size`, i.e. the trailer records where the pixel data ends)
    - width/height as two packed u16s (confirmed: 0x0080/0x0040 = 128/64, and 128*64 = 8192
      matches another field that is exactly the base mip level's byte size for 1-byte/pixel DXT)
    - a mip-level count field
    - a 4-byte format fourCC — literal ASCII **"DXT3"** in this sample
```

This is a huge simplification for the asset pipeline: textures are standard S3TC/DXT-compressed
(Microsoft's DirectX texture compression, well-documented, off-the-shelf decoders exist
everywhere), not a proprietary EA pixel format. The trailer's fourCC field means **the format can
vary per texture** (DXT1/DXT3/DXT5 are all standard EA/DirectX options) — the fourCC must be read
per-texture, not assumed constant.

**Important architectural implication for the 3DS port:** the PICA200 GPU has **no native S3TC/DXT
decoding** — citro3d's supported compressed format is ETC1(+ETC1A4), not DXT. This means the asset
converter (Phase 2) must **transcode** DXT-family data to ETC1/RGBA at build time (offline, on the
PC/WSL side, e.g. with a library like `squish` or a simple custom DXT-block decoder followed by
`tex3ds`/manual ETC1 encoding) — it cannot upload DXT blocks to the 3DS GPU directly.

## Session 7: format survey confirms DXT1/DXT3/uncompressed; trailer format-slot decoded fully

Wrote `tools/scan_texture_formats.py` to walk every `TEXTURES.BIN` under `PC Files/` and tally the
trailer's format field across every texture found (by searching for every `"JDLZ"` magic occurrence
directly, rather than trusting `compressed_len`-based chaining — see below for why). Result across
**60 files / 2208 textures**:

```
DXT3:                    1243
DXT1:                     506
uncompressed A8R8G8B8:    459
```

No DXT5 found anywhere in the retail asset set. This also revealed the trailer's format slot
(12 bytes from the block end, per the layout below) is **not always an ASCII fourCC** — for
uncompressed textures it instead holds a little-endian `u32` `D3DFORMAT` enum value directly (e.g.
`0x15` = 21 decimal = `D3DFMT_A8R8G8B8`). So the field is really a 4-byte union: ASCII fourCC when
block-compressed, raw `D3DFORMAT` integer otherwise — same convention Direct3D8/9 itself uses for
`D3DFORMAT` (FOURCC codes and the enum share the same 32-bit space).

Corrected trailer layout (supersedes the "~156 bytes, partly decoded" description above), reading
backward from the end of a texture's decompressed JDLZ payload:

```
[end-8  .. end)   : 8 bytes, always zero (reserved/padding)
[end-12 .. end-8) : format — ASCII fourCC ("DXT1"/"DXT3"/"DXT5") or u32 D3DFORMAT enum
[end-16 .. end-12): u32 — mip level count (e.g. 6)
[end-20 .. end-16): u32 — unknown, small (e.g. 5) — possibly "mip levels actually stored" or
                     a format-family flag; one less than the mip-count sample seen so far
[end-24 .. end-20): u32 — unknown, small (e.g. 1) — possibly a "is compressed" bool
... texture name (ASCII) and further size/offset fields precede this, as previously documented ...
```

## Superseded: the "one compressed sub-block per mip level" hypothesis was wrong

An earlier pass through this investigation found that chaining to the next texture via
`offset + compressed_len` doesn't land on the next `"JDLZ"` header, and guessed this meant each
mip level was its own separately-tagged (`"JDLZ"` or `"HUFF"`) compressed sub-block within one
texture's span. **This is wrong.** The real explanation (see the `0x33310003` table section below,
found afterward) is much simpler: textures are not packed back-to-back at all — each is addressed
independently via an absolute file offset in the `0x33310003` table, with gaps between them, and
the `"HUFF"`-tagged data found in one such gap simply belonged to the *next texture in file order*
happening to use the alternate HUFF compressor, not to a mip sub-block of the previous one. There is
no confirmed evidence of a multi-block-per-texture mip chain at all.

## Session 7: `tools/dxt_transcode.py` — working DXT1/DXT3/uncompressed -> RGBA8888 transcoder

Precisely pinned down the trailer's fixed-layout fields (all offsets relative to the start of a
156-byte trailer slice taken from the end of a decompressed JDLZ block, `trailer = out[-156:]`):

```
trailer[12:32]   : texture name, ASCII, NUL-padded/truncated to 20 bytes (e.g. "240SX_BADGING")
trailer[36:40]   : u32 hash/CRC
trailer[40:44]   : u32, meaning unknown
trailer[48:52]   : u32, meaning unknown (differs from the next field by exactly trailer[56:60])
trailer[52:56]   : u32, meaning unknown
trailer[56:60]   : u32, meaning unknown
trailer[64:68]   : u32 pixel_data_size — exactly matches len(base-mip pixel data), confirmed
                    (e.g. 8192 = 128*64 bytes for a 128x64 DXT3 base mip at 1 byte/pixel)
trailer[68:70]   : u16 width
trailer[70:72]   : u16 height
trailer[132:136] : u32, small (e.g. 1) — meaning unknown
trailer[136:140] : u32, small (e.g. 5) — meaning unknown
trailer[140:144] : u32 mip level count (e.g. 6)
trailer[144:148] : format — ASCII fourCC ("DXT1"/"DXT3") or u32 D3DFORMAT enum (e.g. 21 = A8R8G8B8)
trailer[148:156] : 8 bytes, always zero
```

Wrote `tools/dxt_transcode.py`, which uses this layout plus a from-scratch DXT1/DXT3 block decoder
(BC1/BC2-style: 4x4 blocks, 2 reference colors + 2-bit-per-pixel interpolated palette for DXT1's RGB,
plus a 4-bit-per-pixel explicit alpha plane for DXT3) and a straight BGRA->RGBA channel swap for
uncompressed `D3DFMT_A8R8G8B8` textures, to decode every texture's **base mip level** (mip levels
1+ are not yet locatable, see the open finding above) to a PNG. **Visually verified correct** against
real output: `240SX_BADGING` decodes to a clean Nissan badge decal with correct alpha transparency,
`240SX_KIT00_HEADLIGH` (DXT1) decodes to a recognizable headlight-glow texture, and
`240SX_DOORLINE` (uncompressed A8R8G8B8, 512x512) decodes to a correctly-colored door-panel texture
sheet. Ran across multiple cars' `TEXTURES.BIN` files (240SX: 44 textures, SUPRA: 47) with no
crashes or garbage output.

This resolves task "write DXT-to-ETC1/RGBA texture transcoder tool" for the RGBA8888 half (the
simplest, correctness-first target per the architectural note above); producing a PICA200-native
ETC1 encoding from this RGBA8888 intermediate is a separate, later optimization step (straightforward
given a correct RGBA source — e.g. via `rg_etc1`/`etcpak`/`tex3ds`), not required to get textures
rendering on real 3DS hardware since raw RGBA8888 is directly usable by citro3d, just less
VRAM-efficient than ETC1.

## Session 7: `0x33310003` per-texture table fully decoded — the real addressing scheme, and the HUFF mystery resolved

Correlating `0x33310003`'s 73 24-byte records against the hash field found in the JDLZ trailer
(`trailer[36:40]`) and testing its other fields as candidate file offsets/sizes revealed the table
is the **authoritative per-texture index** — sequentially scanning for `"JDLZ"` magic (as
`scan_texture_formats.py`/`dxt_transcode.py` currently do) is a lossy shortcut that **silently skips
every texture compressed with the alternate "HUFF" algorithm** (see below). Confirmed 24-byte record
layout:

```c
struct TextureTableEntry {
    uint32_t hash;            // matches 0x33310002's hash and the JDLZ/HUFF trailer's hash field
    uint32_t file_offset;     // ABSOLUTE offset from the start of the .BIN file (not leaf-relative!)
                               // to this texture's compressed-block header ("JDLZ" or "HUFF" magic)
    uint32_t compressed_size; // see below -- exact match for JDLZ, +0x10 for HUFF
    uint32_t uncompressed_size; // exact match against the block header's own uncompressed_len,
                                  // for BOTH JDLZ and HUFF, 100% of 73 samples checked
    uint32_t unknown4;        // constant 0x100 in every sample checked (73/73) -- flags/version?
    uint32_t unknown5;        // constant 0x0 in every sample checked (73/73)
};
```

Verified against every one of the 73 entries in `CARS/240SX/TEXTURES.BIN`: `file_offset` points
exactly at a `"JDLZ"` or `"HUFF"` magic 73/73 times; `uncompressed_size` matches the block header's
own `uncompressed_len` field 73/73 times; `compressed_size` matches the block header's own
`compressed_len` **exactly** for all 44 JDLZ-compressed entries, and is **exactly 0x10 (16) bytes
larger** than the block header's own `compressed_len` for all 29 HUFF-compressed entries (a
consistent, non-coincidental offset across every HUFF sample) — i.e. HUFF-compressed blocks are
preceded by an extra 16-byte wrapper/prefix not counted in the inner header's own `compressed_len`,
while JDLZ blocks have no such wrapper.

**This retroactively explains the session-6 "4468-byte gap with an embedded HUFF marker" finding**:
that gap was never a mip-chain continuation of the *previous* texture — it was simply the **next
texture in file order happening to be HUFF-compressed** rather than JDLZ-compressed, sitting at
whatever the next multiple-of-some-alignment offset the packer chose (textures are not tightly
packed back-to-back; each is written at its own aligned offset per `file_offset`, with gaps in
between that belong to neither texture). The mip-chain-per-block hypothesis from that session is
**wrong and superseded** by this finding — there is no evidence of a multi-block-per-texture mip
chain; each texture is exactly one JDLZ-or-HUFF compressed block (base mip + trailer), addressed
directly via this table.

**Remaining gap**: `HUFF`'s compression algorithm itself has not been ported/implemented (unlike
JDLZ, which was ported from public `nfs-tools`/`OpenNFSTools` prior art) — this blocks decoding
pixel data for the ~40% of textures that use it (29 of 73 in the `240SX` sample). Per-texture
metadata (name, dimensions, format, hash) for HUFF-compressed textures is *not* recoverable without
decompressing, since the trailer lives at the end of the decompressed payload, same as JDLZ.

## Session 8: HUFF traced to its real decompressor in `SPEED2.EXE` via Ghidra; header convention confirmed; algorithm identified as LHA-family adaptive Huffman+LZ

No public prior-art source for EA's "HUFF" format was found to be directly usable: EA's separate
`liblzhl` ("LZH-Light", used in the SAGE/C&C-engine titles, `TheAssemblyArmada/liblzhl` on GitHub) is
a different, fully-adaptive/stateful scheme with no fixed per-block header — wrong algorithm family.
`NFSTools/GlobalLib`'s `HUFF.cs` is a genuine prior decompile of a closely related routine (its
bit-reader and code-length-table-building loop structurally match what was found independently below)
but stops (`return 0;`) before decoding any actual output bytes, so it isn't directly usable either
— though it does corroborate the general algorithm family identified here.

Since no complete usable reference existed, the decompressor was traced directly in the game's own
code via Ghidra headless analysis (`analyzeHeadless`, scripted with Java `GhidraScript`s under
`tools/ghidra_scripts/` — this Ghidra install's `.py` GhidraScript support requires PyGhidra/CPython,
which isn't configured, so Java scripts are the reliable path). The magic checks (`cmp reg,'HUFF'`
etc.) are inline 32-bit immediates in code, not Data-typed strings, so they had to be located via a
raw byte-pattern memory search (`Memory.findBytes`) followed by `getFunctionContaining()` on each hit
— a plain string/xref search finds nothing.

This located the full block-decompression dispatch chain in `SPEED2.EXE`:

- **`FUN_00574670`** — the top-level dispatcher: reads the 4-byte magic and calls the matching
  decompressor: `RAWW` (store, ver 1) → `FUN_005744f0`, `HUFF` (ver 1) → `FUN_005744c0`, `COMP`
  (ver 1, a separate simple LZSS-style scheme, not yet needed/seen in texture data) → `FUN_00574530`,
  `JDLZ` (ver 2) → `FUN_00573700`. `FUN_00573700` was decompiled and confirmed to match
  `tools/jdlz.py`'s existing (already-working) LZRW1/1a-style logic exactly, validating that this
  whole dispatch chain is the real, correct code path (not a decoy/unused routine).
- **`FUN_005744c0`** — the HUFF-specific entry point, confirms magic `"HUFF"` + version byte `1`,
  then calls **`FUN_00573840`**, the real HUFF bitstream decoder.
- **Header-field mapping confirmed via this decompile**: `FUN_005744c0`'s `param_1` is the 16-byte
  block header pointer, read as `int*` — `param_1[0]`=magic, `param_1[1]` low byte=`ver_major` (so
  `ver_minor`/reserved share the rest of that word), `param_1[2]`=`uncompressed_len`. This matches
  the struct already documented for JDLZ, confirming HUFF uses the **same 16-byte header shape**:
  `magic[4]`, `ver_major`(u8), `ver_minor`(u8), `reserved`(u16), `uncompressed_len`(u32 LE),
  `compressed_len`(u32 LE) — **except**, confirmed empirically against a real sample this session,
  **HUFF's `compressed_len` excludes its own 16-byte header** (JDLZ's includes it). Real sample from
  `CARS/240SX/TEXTURES.BIN`, table entry `hash=0x39cbe0b file_offset=0xae480 compressed_size=0x973
  uncompressed_size=0xb3c`: header bytes `48 55 46 46 01 10 00 00 3c 0b 00 00 63 09 00 00` decode as
  `uncompressed_len=0x0b3c` (2876, matches table exactly) and the header's own `compressed_len=0x0963`
  (2403); `0x963 + 0x10 (header) = 0x973` = the table's `compressed_size` exactly. This fully explains
  the previously-documented "table `compressed_size` is exactly 0x10 larger than the block's own
  `compressed_len`, for HUFF only" mystery from the session-7 finding above.

- **`FUN_00573840`** is the actual Huffman-bitstream decoder. Its ~350-line decompilation (captured
  in full in this session's Ghidra output) has a structure that does **not** match JDLZ's simple
  LZRW1 style at all; instead it closely matches the classic **Okumura/Yoshizaki LHA (`-lh*-`)
  adaptive-Huffman-plus-LZ77 decoder family** (the same public-domain algorithm lineage EA is already
  confirmed to have drawn from for its separate `liblzhl` library, and structurally consistent with
  the partial `GlobalLib` decompile above): a code-length histogram is built into a small stack array,
  converted into a canonical "first code per bit-length" cumulative table, then expanded into a fast
  256-entry direct-lookup table; the main decode loop reads either a literal byte or a length+distance
  match via a second Huffman-coded distance/position table, with a distinguished escape symbol driving
  a run-length sub-decode (LHA's classic "special code" scheme for compactly encoding long runs of
  identical code lengths in the table-description header, `read_pt_len`-style); a global flag
  (`DAT_008650f8`, confirmed via Ghidra to be a **zero-initialized `.bss` global**, i.e. always `0` in
  practice, simplifying the branches gated on it to their default path) selects an optional final
  delta/double-delta "un-filter" post-process, which is a texture/pixel-data-specific transform layered
  on top of the general-purpose LHA-style codec (plausibly for better compression of smooth pixel
  gradients) rather than part of the base compression scheme itself.

**Status of the actual port**: the real decompressor function has been located, fully decompiled, and
structurally identified — this was the hard, previously-completely-unknown part. Its full raw
decompilation (as emitted by Ghidra, unedited) is saved at
`docs/formats/huff-FUN_00573840-decompile.c` for reference. A faithful line-by-line Python
transliteration of it (needed because the function's calling convention passes its input-buffer
pointer through the `EAX` register rather than as a visible parameter, and its bit-reader/table-
construction logic is dense, register-optimized C — 32-bit-wraparound arithmetic and masked shift
counts throughout — that must be translated carefully rather than reimplemented from general LHA
knowledge, to guarantee bit-exact compatibility) has **not** yet been completed or tested against real
sample data. This is the concrete next step to fully close task "port/implement EA HUFF decompressor".

A real, verified-correct test sample is saved at
`<scratchpad>/huff_sample_240sx.bin` (2419 bytes, from `CARS/240SX/TEXTURES.BIN` table entry
`hash=0x39cbe0b file_offset=0xae480 compressed_size=0x973 uncompressed_size=0xb3c`) — its header bytes
(`48 55 46 46 01 10 00 00 3c 0b 00 00 63 09 00 00 30 fb 00 0b 3c 99 92 55 38 4a 34 18 0d 44 39 82...`)
were re-verified against the file this session. A correct port must decompress this to exactly 2876
(`0xb3c`) bytes, ending in the same trailer layout already confirmed for JDLZ-compressed textures
above (texture name, hash, dimensions, mip count, format fourCC/D3DFORMAT, 8 trailing zero bytes).

## Session 9: HUFF decompressor completed via direct CPU emulation of the real decoder, not hand-porting

Hand-transliterating `FUN_00573840` (the ~650-line decompiled decoder documented above) was judged too
risky to guarantee bit-exact correctness — it's dense, register-optimized code with 32-bit-wraparound
arithmetic throughout, and a subtly-wrong port could easily produce plausible-looking but corrupt pixel
data that wouldn't be obviously wrong until much later. Instead, since a full raw disassembly dump
(`tools/ghidra_scripts/DumpAsm.java`) confirmed the function makes **zero external calls** (no `CALL`,
`INT`, or `SYSENTER` instructions anywhere in its body), it became possible to execute the real x86
machine code directly via **Unicorn Engine** CPU emulation against the actual bytes of `SPEED2.EXE` —
this guarantees bit-exact correctness by construction, because it *is* the original code running, not a
reimplementation of it.

The calling convention was confirmed by dumping the raw disassembly of the call site in
`FUN_005744c0`: `EAX` at entry = pointer to the compressed payload (`block_start + 0x10`, i.e. right
after the 16-byte HUFF header); the single stack argument = pointer to the output buffer.

This is implemented in **`tools/huff.py`** (`HuffDecompressor` class): it loads `SPEED2.EXE` via
`pefile`, maps each PE section into a Unicorn x86-32 emulator at its real virtual address, allocates a
stack and separate input/output scratch regions (placed well above all real PE sections — see the
`UC_ERR_MAP` note below), then for each HUFF block writes the compressed payload into the input
scratch region, points `EAX`/the output stack slot at the scratch regions, and runs `emu_start` from
`FUN_00573840` to a sentinel return address.

**Validation**: decompressed all 95 HUFF blocks across 6 different cars' `TEXTURES.BIN` files
(240SX 29/29, 3000GT 17/17, 350Z 28/28, A3 21/21, plus two cars with no HUFF blocks present) — zero
decode failures, every output matching its declared `uncompressed_len` exactly and ending in a valid
zero-padded trailer. Scaled up further across the full `PC Files` tree: **2849 total texture blocks
scanned, 641 HUFF-compressed, all 641 decoded with zero errors** (`tools/scan_texture_formats.py`).
Visual pixel-data verification: `240SX_BADGING` (the same real HUFF sample used throughout this
investigation, `CARS/240SX/TEXTURES.BIN` offset `0xae480`) decodes end-to-end through
`tools/dxt_transcode.py`'s existing DXT3 decoder to a clean, correct Nissan 240SX badge/logo decal PNG
— matching the verification bar already established for JDLZ textures in session 7.

Both `tools/dxt_transcode.py` (`iter_textures()`) and `tools/scan_texture_formats.py` (`main()`) now
decode HUFF blocks via `tools/huff.py` in the same code path as JDLZ, rather than skipping/counting
them — 100% of texture blocks in a `TEXTURES.BIN` are now decodable.

One gotcha hit while building this: the emulator's initial scratch-memory addresses (`0x300000`/
`0x700000`) collided with `SPEED2.EXE`'s own `.text` section (`0x401000`-`0x782fff`), causing
`UcError: Invalid memory mapping (UC_ERR_MAP)`. Fixed by relocating scratch memory to `0x02000000`+,
safely clear of all real PE sections (which top out near `0x931fff`).

**Requires**: `pip install unicorn pefile`, and `PC Files/SPEED2.EXE` present on disk (loaded
read-only, never modified) — this approach inherently depends on having the original binary available,
since it executes the original code rather than a reimplementation of it.

## Status: format understanding and decoding complete for both compression schemes

As of session 9, both compression schemes used by `TEXTURES.BIN` (JDLZ ~60%, HUFF ~40%) are fully
decodable, and `dxt_transcode.py`/`scan_texture_formats.py` handle 100% of texture blocks. What remains
open (all low priority, none blocking asset extraction):

1. A handful of trailer u32 fields (`trailer[40:44]`, `trailer[48:60]`, `trailer[132:140]`) and the
   `0x33310003` table's `unknown4`/`unknown5` fields (constant `0x100`/`0x0` in every sample) remain
   unidentified. Only worth chasing if a future bug suggests one of them matters (e.g. a flags bit
   gating some rendering behavior).
2. Mip levels 1+ are not yet handled by `dxt_transcode.py` (only the base mip, i.e.
   `pixel_data[0:pixel_data_size]`, is decoded) — acceptable for a Phase-2 proof-of-concept since
   citro3d can generate its own mipmaps from the base level, but worth revisiting for VRAM/quality
   parity later.

## Session 12: track `.BUN` texture container uses a DIFFERENT, undecoded format

While trying to replace the flat-gray placeholder track backdrop with a real decoded texture
(needed for the Phase 3 drivable-car proof-of-concept), found that `TRACKS/*.BUN` files' embedded
texture data is **not** the same format as `CARS/*/TEXTURES.BIN`, despite using the same outer
`0xb3310000`/`0xb3320000` chunk-type container shape:

- `bin_unpack.py` on `PC Files/TRACKS/STREAML4RR.BUN` finds the expected `0x33320002` bulk
  pixel-data leaf (353432 bytes), but there is **no `0x33310003` per-texture offset table** inside
  its `0xb3310000` info container (only `0x33310001/2/4/5` are present — types `4`/`5` are new,
  not seen in any car `TEXTURES.BIN`, tentatively "envmap-related" per `bin_unpack.py`'s label but
  not investigated further this session).
- Without that table, tried the fallback used for other concatenated-block leaves: scan the
  `0x33320002` payload directly for `"JDLZ"`/`"HUFF"`/`"RAWW"`/`"COMP"` magic. **Found zero matches
  anywhere in the entire 353432-byte leaf.** The payload starts with a 128-byte run of `0x11` filler
  bytes (consistent with the known alignment-padding convention), then continues into what looks
  like genuine compressed/encoded binary data with no recognizable per-block header at all.
- Conclusion: track texture packs use a third, distinct container/compression scheme not yet
  reverse-engineered — possibly a single whole-leaf-level compressed blob (one big JDLZ/HUFF/other
  stream wrapping many textures, rather than one stream per texture), or an entirely different
  on-disk `.tpk` variant used only for streaming/track assets. **Not investigated further this
  session** — this is new-format RE work, not a quick fix, and was correctly scoped out rather than
  forced.
- Workaround taken instead for the Phase 3 proof-of-concept: none yet applied to the running build
  (still flat gray) rather than substituting a car texture, since that would misrepresent "a
  decoded track texture" as done when it isn't. See task list ("Decode and load a real track
  texture") — reopened as a real RE task, not a quick asset-pipeline wiring job as originally
  scoped.

**Next steps for whoever picks this up:** (1) check whether `Speed.elf`'s track/streaming loader
(likely under a `RegionLoader`/streaming-related class per session 3's findings) references a
distinct chunk-type constant or texture-loading function for track vs. car textures — this may
explain the different format faster than blind byte analysis; (2) try treating the whole
`0x33320002` leaf as a single JDLZ or HUFF stream (i.e. call `jdlz.find_header`/`huff` at offset 0
skipping just the leading `0x11` padding, not per-texture) in case the per-texture table was
replaced by one big compressed run instead of removed outright.

## Frontend/global variant — fully decoded (session 2026-08-30): `Global\GlobalTextures.tpk` / `GlobalTexturesPC.tpk`

Task #50 needed real UI/menu texture pixel data (e.g. `U2_MENU_BOX`). These live in a **third
`.tpk` variant**, distinct from both the car (`0x33310003`) and track (session 12, still-undecoded)
schemes — found not via the track loader but by locating the two `.tpk` containers **embedded
directly inside `PC Files/GLOBAL/GLOBALB.BUN`** (a single 8MB file holding two full
`0xb3300000` texture-pack containers back-to-back amid other data, at absolute file offsets
`0x7a80` = `Global\GlobalTextures.tpk` (181 textures) and `0x154280` = `Global\GlobalTexturesPC.tpk`
(30 textures)). Unlike `TEXTURES.BIN`, these containers do **not** sit at offset 0 of their file, so
`scan_texture_formats.py`'s offset-0 leaf walker doesn't find them — a container search has to scan
the whole file for the `0xb3300000` tag.

Confirmed via Ghidra decompile cross-referencing on **both platforms**: GC `Speed_gekko_import.elf`
(`InternalLoadTexturePackHeaderChunks__FP6bChunk`, `AttachTextureInfo__...`,
`AssignTextureData__11TexturePackPcii`) and, this session, the equivalent **PC** `SPEED2.EXE`
functions — located via a literal string search for the surviving debug strings `"TexturePack,%s"`
and `"[Enable 'CodeineInventoryTextures' for this to work"` (the PC binary itself is fully stripped
of symbols, so xref-from-known-string was the only viable path in): `FUN_0048fc40` (ctor, PC mirror
of GC's `800ce94c`), `FUN_00494380` (header-chunk parser, PC mirror of
`InternalLoadTexturePackHeaderChunks`), `FUN_004919e0`/`FUN_00491a30`/`FUN_0048fd30`/`FUN_00488bc0`
(attach/unattach/lookup). Struct-field offsets are byte-identical between GC and PC, consistent with
prior sessions' established precedent that both platforms share the same struct layouts.

### Chunk shape

Same outer container shape as the car variant (`0xb3300000` → `0xb3310000` info + `0xb3320000`
pixel), but the info container holds different leaf types:

- `0x33310001` (124 bytes): name record — `u32` flag/count, then null-padded ASCII strings; the
  one containing a `.`/`\` (e.g. `"Global\GlobalTextures.tpk"`) is the container's real name.
- `0x33310002` (8 bytes/record): hash table, same shape as the car variant.
- **`0x33310004`** (124 bytes/record) — replaces the car variant's `0x33310003` (24 bytes/record).
  One record per texture, confirmed field layout:
  ```c
  struct GlobalTexInfoRecord {           // 0x7c (124) bytes
      char     name[0x18];               // +0x0c, ASCII, NUL-terminated (name starts at +0xc)
      uint32_t hash;                      // +0x24, on-disk texture hash, used directly as lookup key
                                           // (confirmed by PC FUN_0048fd30/FUN_00491a30 reading it
                                           // straight off the record with no runtime computation)
      uint32_t secondary_hash_or_group;   // +0x2c, meaning not fully pinned down
      uint32_t primary_pixel_offset;      // +0x30, ABSOLUTE byte offset into the 0x33320002 leaf
      uint32_t secondary_pixel_offset;    // +0x34, second offset/mip slot (unused, i.e. +0x3c==0,
                                           // on every texture checked so far)
      uint32_t has_primary;               // +0x38, nonzero => primary_pixel_offset is valid
      uint32_t has_secondary;             // +0x3c, nonzero => secondary_pixel_offset is valid
      uint16_t width;                     // +0x44
      uint16_t height;                    // +0x46
      // +0x48 onward: unlabeled (likely mip-count/format-flags/padding), not needed for base-mip
      // extraction and not investigated further; +0x68-+0x7c is runtime-only scratch, zero on disk
  };
  ```
  Confirmed via `AssignTextureData` (GC) / `FUN_0048fd30` (PC): `+0x30`/`+0x34` are gated by
  `+0x38`/`+0x3c` respectively and become the resolved runtime pixel pointer at `+0x70`/`+0x74`.
- **`0x33310005`** (32 bytes/record) — a second, index-aligned table (same count and order as
  `0x33310004`, confirmed by PC `FUN_00494380` asserting the two record counts are equal). Carries
  the compression format:
  ```c
  struct GlobalTexFormatRecord {   // 0x20 (32) bytes
      // +0x00..+0x07: unidentified
      uint8_t  discriminator;      // +0x08, 0 = DXT1, 1 = DXT3 (confirmed on real samples)
      // +0x09..+0x13: unidentified
      char     fourcc[4];          // +0x14, ASCII "DXT1"/"DXT3" (present even though redundant
                                    // with the discriminator byte)
      // +0x18..+0x1f: unidentified
  };
  ```
- `0xb3320000` pixel container: `0x33320001` (24 bytes, summary/count header, same as car variant)
  + **`0x33320002`**, one big leaf holding **all textures' pixel data concatenated, RAW/uncompressed
  — no JDLZ, no HUFF, no per-texture trailer.** This is the key difference from the car format: a
  texture's bytes are `leaf[primary_pixel_offset : primary_pixel_offset + N]`, block-compressed
  (DXT1/DXT3) but with no wrapper at all around that data.

### Recovering the byte length (no size field exists)

Unlike the car format's trailer (which records `pixel_data_size` explicitly), this format stores
**no length anywhere** — `0x33310004` only gives a start offset. The working recipe (implemented in
`tools/extract_global_textures.py`): collect every active (`has_primary != 0`) record, **sort by
`primary_pixel_offset`**, and take each entry's length as the gap to the next entry's offset (the
last entry's length is the leaf's end minus its offset). This was verified two independent ways for
`U2_MENU_BOX` (offset 1010432 in `GlobalTextures.tpk`'s pixel leaf): the DXT3 block-count formula
`(16/4)*(16/4)*16 = 256` bytes, and the gap to the next record by offset
(`U2_MENU_ARROW_LAYER1` @ 1010688) — both give exactly 256. The extractor additionally clamps the
recovered length down to the DXT-formula-expected size when the gap is larger, since trailing gaps
can include unrelated padding.

### Tool and output

`tools/extract_global_textures.py` implements the above end-to-end: scans a file for `0xb3300000`
containers at any byte offset (handles them being embedded inside `GLOBALB.BUN` rather than being
a whole file by themselves), parses both `0x3331000{4,5}` tables, resolves per-texture fourcc,
recovers byte length by the offset-sort trick, and decodes via the existing `tools/dxt_transcode.py`
`decode_dxt()` (reused as-is; note it expects `fourcc` as `bytes`, e.g. `b"DXT3"`, not `str`).

```
python tools/extract_global_textures.py "PC Files/GLOBAL/GLOBALB.BUN" assets/global_textures
Container 0 @ 0x7a80: 'Global\GlobalTextures.tpk', 181 textures
Container 1 @ 0x154280: 'Global\GlobalTexturesPC.tpk', 30 textures
Decoded 211 textures to assets/global_textures
```

Output lives in `assets/global_textures/<container_name>/<name_or_hash>_<fourcc>_<w>x<h>.png` —
211 real, visually-verified-correct PNGs, including `U2_MENU_BOX` and all ~17 of its
corner/arrow/layer/rounded-corner siblings (the frontend UI chrome task #50 needed), plus car
material textures like `CARBONFIBRE`/`CHROME`/`ALUMINUM` and `DECAL_ICON_*` livery icons that also
ship in this "global" pack. Spot-checked visually: `U2_MENU_BOX`/`U2_MENU_ARROW_LAYER1`/
`U2_MENU_ROUNDEDCORNER` decode to coherent soft gradient UI tiles; `DECAL_ICON_ALPINE` decodes to a
recognizable horizontal racing-stripe decal (its banding was initially mistaken for a decode bug,
but the decoded byte count matches the DXT3 formula exactly, confirming it's real artwork, not
corruption).

Remaining open items (none blocking texture extraction): fields `+0x28`, `+0x2c`, and everything
from `+0x48` onward in the `0x33310004` record except the UV scroll fields (next section), plus `+0x00-0x07`/`+0x09-0x13`/`+0x18-0x1f` in the
`0x33310005` record, are unidentified — not investigated since not needed to get correct pixel data
out.

### The UV scroll fields (TASKS.md #269, 2026-10-25)

Six of the `0x33310004` record's unlabeled bytes make a texture scroll. Water,
waterfalls, light strings, rolling neon and the race barriers' red chevrons all
use them. The fields are the same on the PC records and in the GameCube's
memory, and `eDataRender::Render` (Speed.elf `0x8031164C..0x803117D4`) reads
them every time it binds one of these textures:

| Offset | Type | Field | Units |
| --- | --- | --- | --- |
| `+0x52` | u8 | scrollType | 0 none, 1 smooth, 2 stepped, 3 a fixed offset |
| `+0x58` | s16 | ScrollTimeStep | x 1/256 s (`0x80335DB4`): how long a stepped scroll holds |
| `+0x5A` | s16 | ScrollSpeedS | x 1/1024 (`0x80335DAC`) texture widths per second, per step for type 2 |
| `+0x5C` | s16 | ScrollSpeedT | the same along V |
| `+0x5E` | s16 | OffsetS | x 1/1024, negated; type 3 only |
| `+0x60` | s16 | OffsetT | the same along V |

**The time.** It is `WorldTimer` (`0x8046FEE0`, r13 - 0x4f00) x 0.00025
(`0x80335DA8`), in seconds.

**The offset.** `TextureInfo::GetScroll(time, speed, type, step)`
(`0x800CFDAC`) works it out:

- Type 0 returns 0.
- Type 2 first truncates the time to whole steps: `time = (int)(time / step)`.
- Both then return `x - (int)x`, where `x = time * speed`. The truncation runs
  toward zero, so a negative speed gives an offset in (-1, 0].

The render then calls `PSMTXTransApply(S, T, 0)` and `GXLoadTexMtxImm` into
`GX_TEXMTX0`, which adds the offset to the texture coordinate.

**Separate system.** This is not `TextureAnimPack`, the frame-swap animation
(`UpdateTextureAnimations`, `0x800CF2A4`), which the port does not have yet.

**Where it shows up.** 23 textures across the PC data set a scrollType. 9 of
them end up in L4RA's `WORLD.NFSB`. `OBJ_TRACKBARRIERB_NEON_` (hash
`0xEFF8F36A`, `GLOBAL/InGameCommon.bun`, 128x128) is the chevron: type 2,
step 76, speedS -320. S jumps by -0.3125 every 0.297 s, which runs the arrows
along the barrier.

**In the port.**

- `tools/extract_global_textures.py` reads the fields.
- `tools/track_pack.py` writes them into the bank record (+0x16..+0x1f) and
  the type into the material's bits 8-9.
- `model3d.c` (`model3d_set_world_timer`) applies retail's formula to the
  `uvoff` uniform in `scene.v.pica`.
- `main.c`'s `worldTimerTick()` is the WorldTimer. The port advances it only
  in the sim step, so it holds under the pause menu and the overlays.

## `TRACKS/TRACKMAP<id>.BIN` — the frontend variant again, one texture per file

`PC Files/TRACKS/TRACKMAP*.BIN` (129 files, every one exactly 350272 bytes) is **not** a fourth
container format. Each file is a single `0xB3300000` container of exactly the frontend/global
variant decoded above, rooted at file offset 0, named
`Location4\RegionA\Pipeline\PC\TrackMap<id>.tpk` and holding exactly one 512x512 DXT3 texture named
`TRACKMAP<id>` — the Bayview city map with that one track's route drawn over it in white. So
`tools/extract_global_textures.py` and `source/texpack.c` both read them with no changes:

```
python tools/extract_global_textures.py "PC Files/TRACKS/TRACKMAP4011.BIN" out/
Container 0 @ 0x0: 'Location4\RegionA\Pipeline\PC\TrackMap4011.tpk', 1 textures
```

**The alpha channel is empty on every one of them.** Each DXT3 block's alpha half decodes to zero
across all 129 files, even though the `0x33310005` format record's `+0x08` is `1` — the same value a
genuinely translucent DXT3 like `U2_MENU_BOX` carries, and `0` on the DXT1 entries — so nothing in
the metadata marks these as opaque. The whole image is in RGB. Decoded literally the texture is
invisible: the 3DS port's first pass uploaded it as-is and `UI_QRTrackSelect`'s map panel rendered
blank. `texpack_decode_entry()`'s `opaqueIfAlphaEmpty` promotes alpha to 255 only when *every* texel
decoded to 0, which cannot lose an image that was actually there.

### How the frontend binds one

`UI_QRTrackSelect.fng` authors the map panel as an Image widget with object hash `0x90FE80F3` and
resource `Track4000_map.tga` — a placeholder. The engine re-textures that widget per selection:

| routine | what it does |
| --- | --- |
| `0x4E2F30` | track-select ctor; `FindWidget(pkg, 0x90FE80F3)` twice, stores the widget on the map-panel object it constructs at `this+0x118` (vtable `0x78F8C8`, whose only virtual `0x4B4B60` is the deleting destructor) |
| `0x4B53C0` | the per-selection load, called from `0x4CE754`. Track id from `*(short*)([0x863448] + 0x18)`; `sprintf("TRACKS\TRACKMAP%d.bin")` (fmt `.rdata 0x791864`); texture hash from `sprintf("TRACKMAP%d")` (fmt `0x791858`) through `bStringHash` `0x43DB50`; `GetFileSize 0x57D900`; `MemAlloc(size, "TrackMapMemory", 0)` (`0x5E8670`, tag string `0x791848`); `AsyncLoad(path,5,0,0,size)` `0x57CEF0` |
| `0x4B53A0` | that load's completion callback: sets `this+0x50 = 1`, then `0x4901D0(hash, 0, 0)` — the by-hash lookup across every loaded pack |
| `0x4E04AA` | the site that proves the pairing outright: `FindWidget(pkg, 0x90FE80F3)` immediately followed by `SetTextureHash(widget, bStringHash(sprintf("TRACKMAP%d", [0x89E7A0])))` |
| `0x50C990` | `SetTextureHash(widget, hash)` == `widget+0x24 = hash; widget+0x1C \|= 0x400000` |

Confirmed against the shipped data rather than taken on the decompile's word:
`b_string_hash("TRACKMAP4011") == resource_texture_hash("TrackMap4011.tga") == 0xF7600958`, and
`0xF7600958` is exactly the name hash stored at file offset `0xD4` inside `TRACKMAP4011.BIN`. Note
what that identity means: the `.tga` resource path the `.fng` would reference and the bare
`TRACKMAP%d` name the loader builds hash to the same value, because the resource pipeline's hash is
`GetBaseName -> ToUpper -> bStringHash` and these packs already store the upper-cased name.

Ported in `3DS Files/source/trackmap.c` / `.h`.
