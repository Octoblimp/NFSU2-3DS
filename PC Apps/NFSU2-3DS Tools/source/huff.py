"""
EA "HUFF" texture-block decompressor for NFSU2's TEXTURES.BIN.

Unlike tools/jdlz.py (a hand-ported reimplementation of the LZRW1-derived JDLZ
algorithm), HUFF's real decoder (SPEED2.EXE's FUN_00573840, see
docs/formats/huff-FUN_00573840-decompile.c) is a ~650-line, register-optimized,
self-contained (no external calls) x86 routine built around a canonical-Huffman
bit reader whose internal bookkeeping is dense enough that a hand transliteration
risks being subtly wrong in a way that's hard to detect short of exhaustive
testing. Since the routine makes no external calls, it can instead be executed
directly -- via Unicorn CPU emulation, against the real bytes of SPEED2.EXE --
which guarantees bit-exact correctness because it *is* the original code.

Calling convention (confirmed via Ghidra disassembly of the call site in
FUN_005744c0, see docs/formats/texture-tpk.md's Session 8 section):
    EAX                  = pointer to the compressed payload, i.e.
                            block_start + 0x10 (right after the 16-byte header)
    [esp+4] (stack arg)  = pointer to the output buffer
    global @0x008650f8   = always 0 in the shipped binary (confirmed via
                            Ghidra); an optional delta-filter mode selector,
                            inert in practice.

Requires: pip install unicorn pefile
Requires PC Files/SPEED2.EXE to be present (it is loaded read-only, never
modified) -- this tool cannot run without the original binary since it's
executing the original code, not reimplemented logic.
"""
from __future__ import annotations

import os
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

FUNC_ADDR = 0x00573840   # in the US v1.2 SPEED2.EXE; other builds differ
# The decoder's first 45 bytes (sub esp,0x408 ... mov [esp+0xc],ecx). They hold
# no absolute address, so they are the same in every build of the game; the
# EU v1.2 exe has the routine somewhere else and emulating 0x573840 there
# faulted (vinyl_pack's BrokenProcessPool, 2026-11-12). The one global the
# routine reads (0x8650f8 in the US exe) is addressed by its own code, so the
# build's own copy is what it reads.
FUNC_SIG = bytes.fromhex(
    "81ec08040000"      # sub esp, 0x408
    "8b8c240c040000"    # mov ecx, [esp+0x40c]
    "535657"            # push ebx; push esi; push edi
    "8bf8"              # mov edi, eax
    "33c033d233db"      # xor eax,eax; xor edx,edx; xor ebx,ebx
    "3bf8"              # cmp edi, eax
    "8944244c"          # mov [esp+0x4c], eax
    "89542418"          # mov [esp+0x18], edx
    "897c2410"          # mov [esp+0x10], edi
    "894c240c")         # mov [esp+0xc], ecx
IMAGE_SCN_MEM_EXECUTE = 0x20000000


class HuffError(RuntimeError):
    """A HUFF decode or setup failure. Unicorn's UcError cannot be unpickled
    (its __init__ takes errno first), so a worker process that raised one took
    the whole ProcessPoolExecutor down with no message; this one can."""


def find_decoder(pe, image_base):
    """The decoder's VA in this SPEED2.EXE: FUNC_ADDR when the bytes there
    match, else the one place in an executable section that does."""
    hits = []
    for sect in pe.sections:
        if not sect.Characteristics & IMAGE_SCN_MEM_EXECUTE:
            continue
        data = sect.get_data()
        va = image_base + sect.VirtualAddress
        if va <= FUNC_ADDR < va + len(data) and \
                data[FUNC_ADDR - va:FUNC_ADDR - va + len(FUNC_SIG)] == FUNC_SIG:
            return FUNC_ADDR
        at = data.find(FUNC_SIG)
        while at >= 0:
            hits.append(va + at)
            at = data.find(FUNC_SIG, at + 1)
    if len(hits) == 1:
        return hits[0]
    return None


# Address of the RET that ends FUN_005744b9-adjacent code region; used only as
# an emu_start stop address sentinel below (a plain "return to caller" trick),
# not a real call target.
_RET_SENTINEL = 0x00401000

_ALIGN = 0x1000
_SCRATCH_SIZE = 0x00400000  # 4 MiB scratch region for I/O buffers -- comfortably
                              # covers even the largest observed textures (~1.4 MB).


class HuffDecompressor:
    """Loads SPEED2.EXE once and emulates FUN_00573840 for each block."""

    def __init__(self, exe_path: str | Path):
        self._pe = pefile.PE(str(exe_path), fast_load=True)
        image_base = self._pe.OPTIONAL_HEADER.ImageBase
        self._func = find_decoder(self._pe, image_base)
        if self._func is None:
            # Say so here, once, rather than fault on the first block (or, in
            # car_pack, quietly drop every HUFF texture).
            raise HuffError(
                "%s: the HUFF texture decoder is not in this SPEED2.EXE (%d bytes). "
                "Use an unmodified, uncompressed v1.2 exe (US or EU); a packed or "
                "copy-protected exe keeps its code encrypted on disk."
                % (exe_path, os.path.getsize(exe_path)))

        self._mu = Uc(UC_ARCH_X86, UC_MODE_32)
        for sect in self._pe.sections:
            va = image_base + sect.VirtualAddress
            vsize = max(sect.Misc_VirtualSize, sect.SizeOfRawData)
            if vsize == 0:
                continue
            base = va - (va % _ALIGN)
            map_size = ((vsize + (va - base) + _ALIGN - 1) // _ALIGN) * _ALIGN
            self._mu.mem_map(base, map_size)
            data = sect.get_data()[:vsize].ljust(vsize, b"\x00")
            self._mu.mem_write(va, data)

        stack_base = 0x00200000
        stack_size = 0x00100000
        self._mu.mem_map(stack_base, stack_size)
        self._stack_top = stack_base + stack_size - 0x2000

        self._input_addr = 0x02000000
        self._output_addr = self._input_addr + _SCRATCH_SIZE
        self._mu.mem_map(self._input_addr, _SCRATCH_SIZE)
        self._mu.mem_map(self._output_addr, _SCRATCH_SIZE)

    def decompress_payload(self, payload: bytes, uncompressed_len: int) -> bytes:
        """payload = compressed bytes *after* the 16-byte HUFF block header."""
        if uncompressed_len + 0x1000 > _SCRATCH_SIZE:
            raise ValueError(f"uncompressed_len {uncompressed_len} exceeds scratch buffer")

        self._mu.mem_write(self._input_addr, payload + b"\x00" * 256)
        self._mu.mem_write(self._output_addr, b"\x00" * (uncompressed_len + 256))

        esp = self._stack_top
        self._mu.mem_write(esp, _RET_SENTINEL.to_bytes(4, "little"))
        self._mu.mem_write(esp + 4, self._output_addr.to_bytes(4, "little"))
        self._mu.reg_write(UC_X86_REG_ESP, esp)
        self._mu.reg_write(UC_X86_REG_EAX, self._input_addr)

        try:
            self._mu.emu_start(self._func, _RET_SENTINEL, timeout=0, count=0)
            return bytes(self._mu.mem_read(self._output_addr, uncompressed_len))
        except UcError as e:
            raise HuffError("HUFF decode failed at 0x%08x (decoder 0x%08x): %s"
                            % (self._mu.reg_read(UC_X86_REG_EIP), self._func, e)) from None

    def decompress_block(self, block: bytes) -> bytes:
        """block = the full HUFF block including its 16-byte header (magic..compressed_len)."""
        magic = block[0:4]
        if magic != b"HUFF":
            raise ValueError(f"not a HUFF block (magic={magic!r})")
        ver_major = block[4]
        if ver_major != 1:
            raise ValueError(f"unsupported HUFF version {ver_major}")
        uncompressed_len = struct.unpack_from("<I", block, 8)[0]
        compressed_len = struct.unpack_from("<I", block, 12)[0]
        payload = block[16:16 + compressed_len]
        return self.decompress_payload(payload, uncompressed_len)


def decompress_block(exe_path: str | Path, block: bytes) -> bytes:
    """Convenience one-shot wrapper (creates a fresh emulator each call -- for
    bulk decoding of many blocks, construct HuffDecompressor once and reuse it)."""
    return HuffDecompressor(exe_path).decompress_block(block)


if __name__ == "__main__":
    import sys

    import os

    # The PC install: NFSU2_PC_ROOT, else the repo's "PC Files" beside tools/.
    exe = os.path.join(os.environ.get("NFSU2_PC_ROOT") or os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "PC Files"), "SPEED2.EXE")
    bin_path = sys.argv[1] if len(sys.argv) > 1 else None
    off = int(sys.argv[2], 0) if len(sys.argv) > 2 else None
    csize = int(sys.argv[3], 0) if len(sys.argv) > 3 else None

    if bin_path is None:
        print("usage: huff.py <TEXTURES.BIN path> <hex offset> <compressed_size incl header>")
        sys.exit(1)

    with open(bin_path, "rb") as f:
        f.seek(off)
        block = f.read(csize)

    dec = HuffDecompressor(exe)
    out = dec.decompress_block(block)
    print(f"decompressed {len(block)} -> {len(out)} bytes")
    print("trailer tail:", out[-24:].hex())
