#!/usr/bin/env python3
"""Minimal PE section map + x86 disassembly helper for SPEED2.EXE RE.

Usage:
  python tools/pe_disasm.py sections
  python tools/pe_disasm.py dis <VA-hex> [count]     # disassemble from a virtual address
  python tools/pe_disasm.py f2v <fileoff-hex>
  python tools/pe_disasm.py v2f <VA-hex>
"""
import struct, sys
import capstone

PATH = "PC Files/SPEED2.EXE"

class PE:
    def __init__(self, path=PATH):
        self.data = open(path, "rb").read()
        d = self.data
        e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        assert d[e_lfanew:e_lfanew+4] == b"PE\0\0"
        coff = e_lfanew + 4
        machine, nsec, _, _, _, optsize, _ = struct.unpack_from("<HHIIIHH", d, coff)
        opt = coff + 20
        self.imagebase = struct.unpack_from("<I", d, opt + 28)[0]
        self.sections = []
        so = opt + optsize
        for i in range(nsec):
            name, vsize, vaddr, rawsize, rawptr = struct.unpack_from("<8sIIII", d, so + i*40)
            self.sections.append((name.rstrip(b"\0").decode(), vaddr, vsize, rawptr, rawsize))

    def v2f(self, va):
        rva = va - self.imagebase
        for name, vaddr, vsize, rawptr, rawsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                off = rva - vaddr
                if off < rawsize:
                    return rawptr + off
        return None

    def f2v(self, off):
        for name, vaddr, vsize, rawptr, rawsize in self.sections:
            if rawptr <= off < rawptr + rawsize:
                return self.imagebase + vaddr + (off - rawptr)
        return None

    def read_va(self, va, n):
        f = self.v2f(va)
        return self.data[f:f+n] if f is not None else None


def main():
    pe = PE()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "sections"
    if cmd == "sections":
        print(f"imagebase 0x{pe.imagebase:08X}")
        for name, vaddr, vsize, rawptr, rawsize in pe.sections:
            print(f"  {name:8s} VA 0x{pe.imagebase+vaddr:08X} vsize 0x{vsize:X} raw 0x{rawptr:X} rawsize 0x{rawsize:X}")
    elif cmd == "f2v":
        print(hex(pe.f2v(int(sys.argv[2], 16))))
    elif cmd == "v2f":
        print(hex(pe.v2f(int(sys.argv[2], 16))))
    elif cmd == "dis":
        va = int(sys.argv[2], 16)
        count = int(sys.argv[3]) if len(sys.argv) > 3 else 80
        code = pe.read_va(va, count * 8 + 64)
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = False
        n = 0
        for ins in md.disasm(code, va):
            print(f"{ins.address:08X}  {ins.bytes.hex():<20s} {ins.mnemonic:<8s} {ins.op_str}")
            n += 1
            if n >= count:
                break

main()
