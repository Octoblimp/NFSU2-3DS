#!/usr/bin/env python3
"""
Parses a real `.fng`/`FEPackage` screen file (as extracted by fng_extract.py)
into a structured JSON widget-tree dump.

Implements the on-disk format fully reverse-engineered from `Speed.elf`'s
`FEPackageReader` class -- see docs/formats/frontend-menu-re.md for the
decompile-derived spec this directly follows:

- Outer container: `[FourCC:4][Length(LE u32, payload-only):4][Payload]`
  chunks, siblings found by linear scan (payload_start + length).
  Root: `FEn\xE7` -> `PkHd` -> `TypS` -> (`RefP`? `LibR`?) -> `Res\xCC` ->
  `Obj\xCC` { `FOb\xEA` { `ObjD`, `Scrp`*, `MsgR`? } }* -> `PkgR` -> `Targ`.
- Inner `ObjD` interior: `[Tag:2][Length(LE u16):2][Payload]` records,
  walked linearly for the entry's byte range. `Ot` starts a new widget;
  `OP`/`PA`/`SA`/`On`/`Oh` are generic; `SH`/`SL`/`St`/`Sb`/`Sl`/`Sw`/`Sj`
  are String-type; `IF` is Image-type; `LD`/`SI`/`Lj`/`Cc`/`Cs`/`Lt`/`Lv`
  are CodeListBox-type.

Usage:
    python fng_parse.py <file.fng> [out.json]
    python fng_parse.py <file.fng>          # prints JSON to stdout
"""
import json
import struct
import sys

OBJ_TYPE_NAMES = {
    1: "Image", 2: "String", 4: "ListBox", 5: "Group", 6: "CodeListBox",
    9: "Image9", 10: "Image10",
}

# Outer chunk tags that are themselves containers of more outer chunks
# (walked recursively) rather than leaf payload blobs.
CONTAINER_TAGS = {b"FEn\xe7", b"Obj\xcc", b"FOb\xea"}


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def u16(b, off):
    return struct.unpack_from("<H", b, off)[0]


def cstr(b, off):
    end = b.find(b"\x00", off)
    if end == -1:
        end = len(b)
    return b[off:end].decode("latin1")


def parse_objd(payload):
    """Parses one ObjD entry's inner Tag/Len/Payload TLV stream."""
    widget = {"type": None, "typeName": None, "tags": {}}
    off = 0
    n = len(payload)
    while off + 4 <= n:
        tag = payload[off:off + 2].decode("latin1", "replace")
        length = u16(payload, off + 2)
        off += 4
        data = payload[off:off + length]
        off += length

        if tag == "Ot":
            t = u32(data, 0) if len(data) >= 4 else None
            widget["type"] = t
            widget["typeName"] = OBJ_TYPE_NAMES.get(t, f"Unknown({t})")
        elif tag == "OP" and len(data) >= 16:
            # NOT a position pair despite the field names used before this was
            # decompiled -- ReadObjectTags writes field 0 to FEObject+0xc (a
            # cross-package GUID used by FindReferencedObject) and field 1 to
            # +0x10 (shared with the separate Oh tag). Real position/size come
            # from the SA tag below (see docs/formats/frontend-menu-re.md,
            # "position/size finally traced: SA tag").
            widget["guid"] = u32(data, 0)
            widget["objHashField"] = u32(data, 4)
            widget["flags"] = u32(data, 8)
            # FEObject+0x22: index into this package's Res\xCC/RsRq resource-record
            # table (0xFFFF sentinel = cross-package reference, resolved instead via
            # FindReferencedObject at parse time -- see docs/formats/frontend-menu-re.md,
            # "Root-template +0x24 origin"). Resolved to an actual resource name/hash by
            # parse_fng() below via the sibling Res\xCC table, once the whole file's been
            # read (this function only sees one ObjD's bytes, not the package-wide table).
            widget["resourceIndex"] = u16(data, 12)
        elif tag == "On":
            widget["name"] = cstr(data, 0)
        elif tag == "SH" and len(data) >= 4:
            widget["stringHash"] = u32(data, 0)
        elif tag == "SL":
            widget["literalLabel"] = cstr(data, 0)
        elif tag == "If" and len(data) >= 4:
            # Confirmed via ReadObjectTags__15FEPackageReaderP5FETagUl/
            # ProcessImageTag__15FEPackageReaderP5FETag decompile (session 24 part 3):
            # the real on-disk tag is "If" (mixed case), not "IF" -- the old uppercase
            # check here never matched real bytes, so textureHash was never populated.
            # Confirmed target: writes to the live FEImage object at +0x5c (NOT the
            # +0x24 field used by FEngGetTextureHash/FEngSetTextureHash -- those are a
            # separate runtime accessor pair, not what the loader itself writes).
            widget["textureHash"] = u32(data, 0)
        elif tag == "Oh" and len(data) >= 4:
            # NOT a texture hash despite living on Image widgets in real files --
            # confirmed via ReadObjectTags decompile to write into the live FEObject
            # at +0x10, the SAME offset OP's second field ("y") writes to. Real
            # semantics still unclear (object/parent hash?), but this rules out the
            # earlier guess that "Oh" was the texture reference.
            widget["objHash"] = u32(data, 0)
        elif tag == "SA":
            # SA's payload is copied verbatim into the live FEObject's per-instance
            # "track data" buffer starting at buffer offset 0 (ReadObjectTags's SA
            # handler). GetDataOffset__8FEObject18FEKeyTrack_Indices gives each
            # track's byte offset within that buffer: Position (Vector3, track 2)
            # at +0x1c, Rotation (FEQuaternion, track 3) at +0x28, Size (Vector3,
            # track 4) at +0x38 -- see docs/formats/frontend-menu-re.md. Verified
            # against real UI_Main.fng bytes: rotation always decodes as the
            # identity quaternion, size as sane on-screen pixel dimensions.
            widget["tags"].setdefault(tag, []).append(data.hex())
            if len(data) >= 0x28:
                widget["posX"], widget["posDepth"], widget["posZ"] = struct.unpack_from("<3f", data, 0x1c)
            if len(data) >= 0x38:
                widget["rotation"] = struct.unpack_from("<4f", data, 0x28)
            if len(data) >= 0x44:
                sizeW, sizeH, _ = struct.unpack_from("<3f", data, 0x38)
                widget["sizeW"], widget["sizeH"] = sizeW, sizeH
        else:
            # Generic fallback: keep raw bytes (hex) for tags not specially
            # decoded above (PA, SA, Oh, St, Sb, Sl, Sw, Sj, LD, SI, Lj, Cc,
            # Cs, Lt, Lv, etc.) so no information is silently dropped.
            widget["tags"].setdefault(tag, []).append(data.hex())
    return widget


def parse_chunks(buf, start, end):
    """Yields (tag, length, payload_start, payload_end) for each sibling
    chunk in buf[start:end], per the outer FourCC+length format."""
    off = start
    while off + 8 <= end:
        tag = bytes(buf[off:off + 4])
        length = u32(buf, off + 4)
        pstart = off + 8
        pend = pstart + length
        if pend > end or length < 0:
            break
        yield tag, length, pstart, pend
        off = pend


def walk(buf, start, end, node):
    for tag, length, pstart, pend in parse_chunks(buf, start, end):
        tag_str = tag.decode("latin1")
        if tag == b"PkHd":
            node["header"] = parse_pkhd(buf, pstart, pend)
        elif tag == b"TypS":
            node["typeSizes"] = parse_typs(buf, pstart, pend)
        elif tag == b"Res\xcc":
            node["resources"] = parse_res(buf, pstart, pend)
        elif tag in CONTAINER_TAGS:
            child = {}
            walk(buf, pstart, pend, child)
            node.setdefault("_containers", []).append({"tag": tag_str, **child})
        elif tag == b"ObjD":
            node["widget"] = parse_objd(buf[pstart:pend])
        elif tag == b"Scrp":
            node.setdefault("scripts", []).append(buf[pstart:pend].hex())
        elif tag == b"MsgR":
            node["messageResponses"] = buf[pstart:pend].hex()
        else:
            node.setdefault("_other", []).append(
                {"tag": tag_str, "length": length}
            )


def parse_pkhd(buf, start, end):
    # Confirmed against real bytes (GenericDialog_SMALL.fng): a fixed 24-byte
    # header (+0 version stamp 0x00020000, +4/+8/+12 flags/checksum-ish
    # fields not yet semantically pinned down, +16 Name length incl. NUL,
    # +20 Filename length incl. NUL), then the Name C-string, then the
    # Filename C-string immediately after Name's NUL terminator. The next
    # sibling chunk (TypS) starts at this chunk's declared length regardless
    # of any padding after the strings, so exact trailing-byte semantics
    # don't affect sibling walking.
    hdr = {}
    if end - start >= 24:
        hdr["version"] = u32(buf, start + 0)
        hdr["field_4"] = u32(buf, start + 4)
        hdr["field_8"] = u32(buf, start + 8)
        hdr["field_12"] = u32(buf, start + 12)
    name_off = start + 24
    hdr["name"] = cstr(buf, name_off)
    filename_off = name_off + len(hdr["name"]) + 1
    hdr["filename"] = cstr(buf, filename_off)
    return hdr


def parse_typs(buf, start, end):
    entries = []
    off = start
    while off + 8 <= end:
        obj_type = u32(buf, off)
        size = u32(buf, off + 4)
        entries.append({"type": obj_type, "typeName": OBJ_TYPE_NAMES.get(obj_type, f"Unknown({obj_type})"), "size": size})
        off += 8
    return entries


def parse_res(buf, start, end):
    """Parses the Res\\xCC chunk: a RsNm flat string blob (resource names,
    NUL-separated, walked by byte offset -- NOT array index) followed by an
    RsRq record table. Record layout confirmed against Callback__17Resource
    ConnectorP8FEObject's decompile (docs/formats/frontend-menu-re.md,
    "Root-template +0x24 origin") AND empirically against UI_Main.fng's raw
    bytes: [count:u32] then `count` 24-byte records
    {hash:u32, nameOffset:u32, kind:u32, field3:u32, field4:u32, field5:u32}.
    nameOffset indexes directly into the RsNm blob (byte offset, not a
    string-array index). field4/field5 (record+0x10/+0x14, the fields the
    live ResourceConnector copies into FEObject+0x24/+0x28) are 0 on disk in
    every record observed so far -- they appear to be runtime-populated
    (filled in once the named resource is actually loaded/hashed), not
    stored in the file. `hash` (record+0x0) is a precomputed hash of the
    resource name and is what's actually usable offline for texture lookup.
    """
    name_blob = b""
    records = []
    for tag, length, pstart, pend in parse_chunks(buf, start, end):
        if tag == b"RsNm":
            name_blob = buf[pstart:pend]
        elif tag == b"RsRq":
            rq = buf[pstart:pend]
            if len(rq) >= 4:
                count = u32(rq, 0)
                for i in range(count):
                    roff = 4 + i * 24
                    if roff + 24 > len(rq):
                        break
                    h, name_off, kind, f3, f4, f5 = struct.unpack_from("<6I", rq, roff)
                    records.append({
                        "index": i,
                        "hash": h,
                        "nameOffset": name_off,
                        "name": cstr(name_blob, name_off) if name_off < len(name_blob) else None,
                        "kind": kind,
                        "field3": f3,
                        "field4": f4,
                        "field5": f5,
                    })

    # Legacy flat name-list form, kept for anything still consuming it.
    parts = name_blob.split(b"\x00")
    names = [p.decode("latin1", "replace") for p in parts if p]

    return {"names": names, "records": records}


def b_string_hash(s):
    """djb2-style hash used by bStringHash__FPCc (Speed.elf @ 8005dbe0)."""
    h = 0xFFFFFFFF
    for ch in s.encode("latin1"):
        h = (ch + h * 0x21) & 0xFFFFFFFF
    return h


def resource_texture_hash(name):
    """C port of LoadResources__5cFEngP9FEPackagelP17FEResourceRequest's
    texture-pack hash computation (Speed.elf @ 8014d644): GetBaseName (strip
    directory AND extension) -> ToUpper (ASCII a-z only) -> bStringHash. This
    is the hash GetTextureInfo/RenderImage actually key on at runtime -- NOT
    the RsRq record's on-disk `hash` field, which lives in a different,
    package-local hash space. Verified: "U2_Menu_Box.tga" -> 0xa67c3fe2,
    matching GLOBALB.BUN's stored hash for its "U2_MENU_BOX" entry.
    """
    base = name.replace("\\", "/").rsplit("/", 1)[-1]
    if "." in base:
        base = base.rsplit(".", 1)[0]
    base = "".join(c.upper() if "a" <= c <= "z" else c for c in base)
    return b_string_hash(base)


def resolve_resource_indices(node, table):
    if table is None:
        return
    records = table.get("records", [])
    widget = node.get("widget")
    if widget is not None and "resourceIndex" in widget:
        idx = widget["resourceIndex"]
        if idx == 0xFFFF:
            widget["resourceRef"] = "cross-package (0xFFFF sentinel, resolved via FindReferencedObject)"
        elif idx < len(records):
            rec = records[idx]
            widget["resourceRef"] = {"hash": rec["hash"], "name": rec["name"], "kind": rec["kind"]}
            if rec["name"]:
                widget["textureHashResolved"] = resource_texture_hash(rec["name"])
        else:
            widget["resourceRef"] = f"out of range (index {idx}, table has {len(records)} records)"
    for c in node.get("_containers", []):
        resolve_resource_indices(c, table)


def collect_widgets(node, out):
    if "widget" in node:
        out.append(node["widget"])
    for c in node.get("_containers", []):
        collect_widgets(c, out)


def parse_fng(path):
    with open(path, "rb") as f:
        buf = f.read()

    if buf[0:4] != b"FEn\xe7":
        raise ValueError(f"{path}: missing FEn\\xE7 magic, got {buf[0:4]!r}")

    root = {}
    walk(buf, 0, len(buf), root)

    # The whole file is one outer FEn\xE7 container chunk; unwrap it so
    # header/typeSizes/resources/widgets are exposed directly rather than
    # nested under root["_containers"][0].
    fen = root["_containers"][0]

    resolve_resource_indices(fen, fen.get("resources"))

    widgets = []
    collect_widgets(fen, widgets)

    return {
        "file": path,
        "header": fen.get("header"),
        "typeSizes": fen.get("typeSizes"),
        "resources": fen.get("resources"),
        "widgetCount": len(widgets),
        "widgets": widgets,
    }


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        sys.exit(1)

    result = parse_fng(sys.argv[1])
    text = json.dumps(result, indent=2)

    if len(sys.argv) == 3:
        with open(sys.argv[2], "w") as f:
            f.write(text)
        print(f"wrote {sys.argv[2]} ({result['widgetCount']} widgets)")
    else:
        print(text)


if __name__ == "__main__":
    main()
