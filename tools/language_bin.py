#!/usr/bin/env python3
"""
Decoder for NFSU2's LANGUAGES/*.bin string tables (bChunk type 0x00039000).

Format confirmed via:
  - tools/bin_unpack.py chunk walk on PC Files/LANGUAGES/English.bin (single
    0x39000 leaf, 250416 bytes).
  - Cross-checked against yugecin/nfsu2-re (community RE project,
    nfsu2-re-binfiles/main.c's read_language_strings()), which documents the
    exact struct layout from a live decompile of the game's own loader.
  - Independently verified: the wchar remap table's non-identity entries
    matched byte-for-byte against our own hex dump of the file before
    consulting the reference source. Note they are NOT Windows-1252: English
    .bin uses 0x81 -> U+2019, 0x82 -> U+2122, 0x83 -> U+2013, 0x84 -> U+201D,
    0x86 -> U+2026, 0x87 -> U+201C, 0x88 -> U+201E, 0x89-0x8C -> four Hangul
    syllables, and 0xA9/0xAE/0xD1 mapped to themselves. 16 non-zero entries in
    all, affecting 62 of the 3925 strings.
  - Spot-checked against extracted/fng_parsed/LS_Demo_Legal.json: the legal
    screen's third String widget carries stringHash=2531555110 (0x96e47726)
    with a literal UTF-16LE fallback string embedded in its "St" tag; the
    same hash resolves via this decoder to byte-identical English.bin text.

Struct layout (all fields little-endian, offsets relative to the start of
the 0x39000 leaf's payload, i.e. right after its 8-byte {type,size} chunk
header):

    struct Header {
        uint32_t wcharTableOffset;  // always 0x10 in samples seen
        uint32_t numStrings;
        uint32_t tableOffset;       // offset of TableEntry[numStrings]
        uint32_t stringsOffset;     // offset of the string blob
    };
    struct WcharTable {            // at wcharTableOffset
        int32_t  something;         // 256 in samples seen
        uint16_t entries[256];      // byte value -> unicode codepoint remap
    };
    struct TableEntry {            // at tableOffset, numStrings of these,
        uint32_t hash;              // sorted low->high hash (binary search)
        int32_t  stringOffset;      // relative to stringsOffset
    };
    // strings blob at stringsOffset: back-to-back NUL-terminated strings,
    // stored raw-byte (not UTF-16). The wchar table IS the encoding for those
    // bytes -- an earlier version of this note called it a mere font-glyph
    // remap you could ignore, and that is what left the port rendering
    // "EA GAMES<0x82> TRAX" as "EA GAMES? TRAX" (TASKS.md #128). Decode
    // through it, as decode_string() below and source/langbin.c both do.
"""
import struct
import sys


def find_leaf_0x39000(data: bytes):
    """Walk the top-level bChunk stream and return the raw payload bytes of
    the first type==0x39000 leaf (language string table)."""
    off = 0
    while off + 8 <= len(data):
        ctype, csize = struct.unpack_from("<II", data, off)
        payload_off = off + 8
        if ctype == 0x00039000:
            return data[payload_off:payload_off + csize]
        off = payload_off + csize
    raise ValueError("no 0x39000 (language string table) leaf found")


class LanguageTable:
    def __init__(self, payload: bytes):
        wchar_table_off, num_strings, table_off, strings_off = \
            struct.unpack_from("<4I", payload, 0)
        self.num_strings = num_strings
        self.wchar_remap = struct.unpack_from(
            "<256H", payload, wchar_table_off + 4)

        entries = []
        pos = table_off
        for _ in range(num_strings):
            h, str_off = struct.unpack_from("<Ii", payload, pos)
            pos += 8
            entries.append((h, str_off))

        strings_blob = payload[strings_off:]
        self.by_hash = {}
        for h, str_off in entries:
            end = strings_blob.find(b"\x00", str_off)
            self.by_hash[h] = self.decode_string(strings_blob[str_off:end])

        # sanity: table is documented as sorted low->high hash for binary search
        self.sorted_ok = entries == sorted(entries, key=lambda e: e[0])

    def decode_string(self, raw: bytes) -> str:
        """Decode one raw string through the file's own byte -> codepoint
        table. A zero entry means no remap, so the byte is its own codepoint
        (Latin-1); this matches source/langbin.c."""
        return "".join(chr(self.wchar_remap[b] or b) for b in raw)

    def get(self, h: int):
        return self.by_hash.get(h & 0xFFFFFFFF)


def load(path: str) -> LanguageTable:
    data = open(path, "rb").read()
    return LanguageTable(find_leaf_0x39000(data))


def main():
    if len(sys.argv) < 2:
        print("usage: language_bin.py <path to English.bin> [--dump out.txt] [--hash 0xXXXXXXXX]")
        return 1
    path = sys.argv[1]
    table = load(path)
    print(f"{path}: {table.num_strings} strings, sorted={table.sorted_ok}")

    if "--dump" in sys.argv:
        out_path = sys.argv[sys.argv.index("--dump") + 1]
        with open(out_path, "w", encoding="utf-8") as f:
            for h in sorted(table.by_hash):
                f.write(f"{h:08X}: {table.by_hash[h]}\n")
        print(f"dumped {len(table.by_hash)} strings to {out_path}")

    if "--hash" in sys.argv:
        hs = sys.argv[sys.argv.index("--hash") + 1]
        h = int(hs, 16)
        print(f"{h:08X}: {table.get(h)!r}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
