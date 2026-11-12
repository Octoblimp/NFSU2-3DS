# Prints a Luma3DS exception dump (crash_dump_*.dmp): the header, the
# registers, the code words before PC and the stack, plus the addr2line of PC
# and LR when the .elf is given (#306).
#
# Usage: python tools/luma_dump.py "3DS Files/crash_dump_00000000.dmp" ["3DS Files/NFSU2-3DS.elf"]
#
# The .elf has to be the build that crashed: check the code words printed
# here against `arm-none-eabi-objdump -d` at PC before trusting the lines.
import os, shutil, struct, subprocess, sys

d = open(sys.argv[1], 'rb').read()
magic = struct.unpack_from('<II', d, 0)
vmin, vmaj, proc, core = struct.unpack_from('<HHHH', d, 8)
typ, total, regSz, codeSz, stackSz, addSz = struct.unpack_from('<IIIIII', d, 0x10)
kinds = {0: 'FIQ', 1: 'undefined instruction', 2: 'prefetch abort', 3: 'data abort'}
print('magic %08x %08x  Luma dump v%d.%d  ARM%d core %d  type %d (%s)' %
      (magic[0], magic[1], vmaj, vmin, proc, core, typ, kinds.get(typ, '?')))
names = ['r%d' % i for i in range(13)] + ['sp', 'lr', 'pc', 'cpsr', 'dfsr', 'ifsr', 'far',
                                          'fpexc', 'fpinst', 'fpinst2']
off = 0x28
regs = struct.unpack_from('<%dI' % (regSz // 4), d, off)
for i, v in enumerate(regs):
    print('  %-7s %08x' % (names[i] if i < len(names) else '?%d' % i, v))
off += regSz
code = d[off:off + codeSz]
off += codeSz
print('code words, the last at pc:')
words = ['%08x' % struct.unpack_from('<I', code, i)[0] for i in range(0, len(code), 4)]
for i in range(0, len(words), 8):
    print('  ' + ' '.join(words[i:i + 8]))
stack = d[off:off + stackSz]
print('stack from sp %08x (first 32 words):' % regs[13])
for i in range(0, min(len(stack), 128), 4):
    print('  %08x: %08x' % (regs[13] + i, struct.unpack_from('<I', stack, i)[0]))

if len(sys.argv) > 2:
    # DEVKITARM is often the msys spelling (/opt/devkitpro/...), which Windows
    # Python cannot open, so the usual install path is tried too.
    cands = [shutil.which('arm-none-eabi-addr2line')] + [
        os.path.join(base, 'bin', 'arm-none-eabi-addr2line.exe')
        for base in (os.environ.get('DEVKITARM', ''), 'C:/devkitPro/devkitARM') if base]
    tool = next((c for c in cands if c and os.path.exists(c)), 'arm-none-eabi-addr2line')
    out = subprocess.run([tool, '-f', '-i', '-e', sys.argv[2], '0x%x' % regs[15], '0x%x' % regs[14]],
                         capture_output=True, text=True)
    print('addr2line pc, lr:')
    print(out.stdout.rstrip() or out.stderr.rstrip())
