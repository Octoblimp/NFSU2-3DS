# The car-parts database (`GLOBALB.BUN`, chunk `0x80034602`)

## What it is for

A save stores no colour. `PresetCarSlot`'s `BASE_PAINT` slot holds the bStringHash of a *part
name* — `GLOSS_L3_TEST20`, `RIMS_L1_COLOR08` — and the actual RGB lives in that part's attribute
list here. So the chain from "the player picked dark blue" to "the body draws dark blue" is:

```
save +0x1C+4*0x40  ->  part-name hash  ->  parts DB record  ->  attr list  ->  RED/GREEN/BLUE
```

Every link below was recovered from retail data and cross-checked; none of it is guessed.
`docs/formats/savefile.md` covers the save side, this covers the database side.

## Container layout

`GLOBALB.BUN` chunk `0x80034602` holds six children. **Walk by chunk id, never by absolute
offset** — `tools/paint_pack.py` does, and validates the header's counts against the actual
child sizes before trusting either.

| Chunk | Size | Contents |
|---|---|---|
| `0x00034603` | 60 B | header; `u32[8]` = attribute count (4636), `u32[14]` = part count (12167) |
| `0x00034606` | 36 860 B | string pool (part and attribute-key names) |
| `0x0003460c` | 47 620 B | attribute-index lists, `u16` |
| `0x00034605` | 4636 × 8 B | attributes, `{u32 keyHash, u32 value}` |
| `0x0003460a` | 1580 × 36 B | model table: which model(s) a part draws, see below |
| `0x0003460b` | 300 B | car types: `u32` name hashes, the base names for selector 1 |
| `0x00034604` | 12167 × 14 B | part records |

### Part record (14 B)

```
+0x00  u32  nameHash      bStringHash of the part name
+0x04  u8   slot          the car slot the part fits
+0x05  u8   flags         >> 5 is the upgrade level, 0 = stock
+0x06  u8   baseSel       model base name: 0 none, 1 car type, else the 0xebb03e66 attribute
+0x07  u8   carType       index into 0x3460b, for baseSel 1
+0x08  u16  nameOff4      pool byte offset / 4  -- *4 reproduces the exact pool offset
+0x0a  u16  attrList      index into 0x3460c, at a COUNT, then that many u16 attr indices
+0x0c  u16  model         index into 0x3460a, 0xffff = the part draws nothing
```

`nameOff4 * 4` was confirmed to land on an exact NUL-terminated pool string for all 12167
records, which is what pins the field down.

### Attribute keys

Keys are the bStringHash of names that appear in the database's *own* string pool, so they
reverse-resolve without any external table:

`RED` (`0x0000d99a`), `GREEN` (`0x02ddc8f0`), `BLUE` (`0x00136707`), `TEXTURE` (`0xfd35fe70`),
`DISPRED`/`DISPGREEN`/`DISPBLUE`, `SPINNER_TEXTURE`, `HUDINDEX`, `NUM_DECALS`, `NUMCOLOURS`
(`0x564b8cb6`), `NUMREMAPCOLOURS` (`0x6212682b`), `REMAP` (`0x039dd714`),
`HOODHUE`/`HOODSAT`/`HOODLUM`, `TIREHUE`/`TIRESAT`/`TIRELUM`, `ALPHA`, `ANIMSTYLE`.

`bStringHash` is djb2 with seed `0xFFFFFFFF` and multiplier 33 over the **upper-cased** name
(`tools/track_pack.py:594`).

## What the port ships

Walking this database at runtime would need the whole 8 MB bundle resident, so
`tools/paint_pack.py` does it offline and writes `GLOBAL/PAINT.nfsp`:

```
0x00  magic 'NFSP'
0x04  version (1)
0x08  entry count
0x0c  reserved
0x10  count * { u32 nameHash, u8 r, u8 g, u8 b, u8 pad }   sorted by hash
```

**494 paint parts, 3968 bytes**, one binary search per lookup (`3DS Files/source/paint.c`).

### A trap in building that table

43 of those 494 parts have a pool string that is a **display name**, not the hashed name — the
neon colours (`RED`, `GREEN`, `BLUE`, `YELLOW`, `AQUA`, `WHITE`, `CORAL`, `MAGENTA`, `LIME`,
`PURPLE`, `GOLD` and their `PULSE` variants) plus `CUSTOMPAINT_1..20`. Validating
`bStringHash(poolName) == record.nameHash` and dropping mismatches therefore silently deletes
every neon colour in the game. The record's own hash field is the key; a mismatch is worth
counting, not rejecting.

## Which model a part draws (`0x3460a`)

This is `CarPartModelTable::GetModelNameHash(part, model, lod)`. A part has up to
two models (0 and 1) at four LODs (A-D). Model-table record, 36 B:

```
+0x00  u8   templated
+0x01  u8   ?
+0x02  u16  s2            pool index / 4 of a middle name piece, 0xffff = none
+0x04  u32  v[8]          v[model * 4 + lod]; 0xffffffff = no such model
```

Not templated: `v` is the model hash itself. Templated: `v` is a pool index / 4 and
the hash is built by carrying bStringHash on from a seed (`ext(h, s)` runs the same
djb2 loop from `h`):

```
base  = 0xffffffff (baseSel 0) | carTypes[carType] (1) | attr 0xebb03e66 (else)
hash  = ext(ext(ext(base, pool[s2]), pool[v]), "_" + chr('A' + lod))
```

Seven part hashes appear twice in the record table (`RED`, `GREEN`, ..., `STOCK`), all
colour parts with no model, so which copy wins does not matter. `tools/kit_pack.py`
(`PartsDb.model_hash`) refuses a repeat that draws a model rather than guess.

The port resolves all of this offline into `GLOBAL/PARTMODELS.nfsm` (part hash ->
model hashes, the kit each lives in, level, slot, light texture base), and
`3DS Files/source/carkit.c` builds the career car from it. The kit formats and the
placement rules Render__13CarRenderInfo applies are in `kit_pack.py`'s docstring and
`carkit.c`; TASKS.md #204 has the evidence.

## Vinyls are not textures

The four `VINYL_LAYER<n>` slots resolve to real parts here (`LIQUIDS_096`, `TEAR_012`,
`LIGHTNG_175`), but those names occur **nowhere else in the entire retail tree** — only in this
string pool. Neither `GLOBALA` nor `GLOBALB`'s TPK contains their hashes, so there is no bitmap
to load: NFSU2's vinyl art is vector shape data. Candidate chunks, not yet decoded:
`0x80034a10` (sizes `0x116b0` and `0x6410`) and `0x80034a30` (`0x5180`).

Until a rasteriser exists, `ProfileCarRecord.vinylLayer[4]` is decoded and carried but nothing
draws it. Per this project's standing rule that is left visibly missing rather than faked.

## Related

- `docs/formats/savefile.md` — `PresetCarSlot` and the `CarSlotId` enum.
- `tools/paint_pack.py` — the packer.
- `3DS Files/source/paint.c` — the runtime lookup.
- `3DS Files/source/carskin.c` — how the paint colour is actually applied: retail's
  CompositeSkin, for every car (its kit, or its stock `.nfsc`).
- `tools/kit_pack.py`, `3DS Files/source/carkit.c` — the part models and how the career
  car is assembled from them.
